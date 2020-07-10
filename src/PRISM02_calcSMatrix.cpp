// Copyright Alan (AJ) Pryor, Jr. 2017
// Transcribed from MATLAB code by Colin Ophus
// Prismatic is distributed under the GNU General Public License (GPL)
// If you use Prismatic, we kindly ask that you cite the following papers:

// 1. Ophus, C.: A fast image simulation algorithm for scanning
//    transmission electron microscopy. Advanced Structural and
//    Chemical Imaging 3(1), 13 (2017)

// 2. Pryor, Jr., A., Ophus, C., and Miao, J.: A Streaming Multi-GPU
//    Implementation of Image Simulation Algorithms for Scanning
//	  Transmission Electron Microscopy. arXiv:1706.08563 (2017)

#include "PRISM02_calcSMatrix.h"
#include "params.h"
#include <iostream>
#include <vector>
#include <thread>
#include "fftw3.h"
#include <mutex>
#include "ArrayND.h"
#include <complex>
#include "utility.h"
#include "configure.h"
#include "WorkDispatcher.h"
#include "fileIO.h"
#ifdef PRISMATIC_BUILDING_GUI
#include "prism_progressbar.h"
#endif

namespace Prismatic
{

using namespace std;
const PRISMATIC_FLOAT_PRECISION pi = acos(-1);
const std::complex<PRISMATIC_FLOAT_PRECISION> i(0, 1);

void setupCoordinates(Parameters<PRISMATIC_FLOAT_PRECISION> &pars)
{

	// setup some Fourier coordinates and propagators
	pars.imageSize[0] = pars.pot.get_dimj();
	pars.imageSize[1] = pars.pot.get_dimi();
	Array1D<PRISMATIC_FLOAT_PRECISION> qx = makeFourierCoords(pars.imageSize[1], pars.pixelSize[1]);
	Array1D<PRISMATIC_FLOAT_PRECISION> qy = makeFourierCoords(pars.imageSize[0], pars.pixelSize[0]);

	pair<Array2D<PRISMATIC_FLOAT_PRECISION>, Array2D<PRISMATIC_FLOAT_PRECISION>> mesh = meshgrid(qy, qx);
	pars.qya = mesh.first;
	pars.qxa = mesh.second;
	Array2D<PRISMATIC_FLOAT_PRECISION> q2(pars.qya);
	transform(pars.qxa.begin(), pars.qxa.end(),
			  pars.qya.begin(), q2.begin(), [](const PRISMATIC_FLOAT_PRECISION &a, const PRISMATIC_FLOAT_PRECISION &b) {
				  return a * a + b * b;
			  });
	pars.q2 = q2;

	// get qMax
	long long ncx = (long long)floor((PRISMATIC_FLOAT_PRECISION)pars.imageSize[1] / 2);
	PRISMATIC_FLOAT_PRECISION dpx = 1.0 / ((PRISMATIC_FLOAT_PRECISION)pars.imageSize[1] * pars.meta.realspacePixelSize[1]);
	long long ncy = (long long)floor((PRISMATIC_FLOAT_PRECISION)pars.imageSize[0] / 2);
	PRISMATIC_FLOAT_PRECISION dpy = 1.0 / ((PRISMATIC_FLOAT_PRECISION)pars.imageSize[0] * pars.meta.realspacePixelSize[0]);
	pars.qMax = std::min(dpx * (ncx), dpy * (ncy)) / 2;

	// construct anti-aliasing mask
	pars.qMask = zeros_ND<2, unsigned int>({{pars.imageSize[0], pars.imageSize[1]}});
	{
		long offset_x = pars.qMask.get_dimi() / 4;
		long offset_y = pars.qMask.get_dimj() / 4;
		long ndimy = (long)pars.qMask.get_dimj();
		long ndimx = (long)pars.qMask.get_dimi();
		for (long y = 0; y < pars.qMask.get_dimj() / 2; ++y)
		{
			for (long x = 0; x < pars.qMask.get_dimi() / 2; ++x)
			{
				pars.qMask.at(((y - offset_y) % ndimy + ndimy) % ndimy,
							  ((x - offset_x) % ndimx + ndimx) % ndimx) = 1;
			}
		}
	}

	// build propagators
	pars.prop = zeros_ND<2, std::complex<PRISMATIC_FLOAT_PRECISION>>({{pars.imageSize[0], pars.imageSize[1]}});
	pars.propBack = zeros_ND<2, std::complex<PRISMATIC_FLOAT_PRECISION>>({{pars.imageSize[0], pars.imageSize[1]}});
	for (auto y = 0; y < pars.qMask.get_dimj(); ++y)
	{
		for (auto x = 0; x < pars.qMask.get_dimi(); ++x)
		{
			if (pars.qMask.at(y, x) == 1)
			{
				pars.prop.at(y, x) = exp(-i * pi * complex<PRISMATIC_FLOAT_PRECISION>(pars.lambda, 0) *
										 complex<PRISMATIC_FLOAT_PRECISION>(pars.meta.sliceThickness, 0) *
										 complex<PRISMATIC_FLOAT_PRECISION>(pars.q2.at(y, x), 0));
				
				//propBack is only used to center defocus of HRTEM at center of cell
				pars.propBack.at(y, x) = exp(i * pi * complex<PRISMATIC_FLOAT_PRECISION>(pars.lambda, 0) *
											 complex<PRISMATIC_FLOAT_PRECISION>(pars.tiledCellDim[0] / 2, 0) *
											 complex<PRISMATIC_FLOAT_PRECISION>(pars.q2.at(y, x), 0));
			}
		}
	}
}

inline void setupBeams(Parameters<PRISMATIC_FLOAT_PRECISION> &pars)
{
	// determine which beams (AKA plane waves, or Fourier components) are relevant for the calculation

	Array1D<PRISMATIC_FLOAT_PRECISION> xv = makeFourierCoords(pars.imageSize[1],
															  (PRISMATIC_FLOAT_PRECISION)1 / pars.imageSize[1]);
	Array1D<PRISMATIC_FLOAT_PRECISION> yv = makeFourierCoords(pars.imageSize[0],
															  (PRISMATIC_FLOAT_PRECISION)1 / pars.imageSize[0]);
	pair<Array2D<PRISMATIC_FLOAT_PRECISION>, Array2D<PRISMATIC_FLOAT_PRECISION>> mesh_a = meshgrid(yv, xv);

	// create beam mask and count beams
	Prismatic::Array2D<unsigned int> mask;
	mask = zeros_ND<2, unsigned int>({{pars.imageSize[0], pars.imageSize[1]}});
	pars.numberBeams = 0;
	long interp_fx = (long)pars.meta.interpolationFactorX;
	long interp_fy = (long)pars.meta.interpolationFactorY;
	for (auto y = 0; y < pars.qMask.get_dimj(); ++y)
	{
		for (auto x = 0; x < pars.qMask.get_dimi(); ++x)
		{
			if (pars.q2.at(y, x) < pow(pars.meta.alphaBeamMax / pars.lambda, 2) &&
				pars.qMask.at(y, x) == 1 &&
				(long)round(mesh_a.first.at(y, x)) % interp_fy == 0 &&
				(long)round(mesh_a.second.at(y, x)) % interp_fx == 0)
			{
				mask.at(y, x) = 1;
				++pars.numberBeams;
			}
		}
	}

	// number the beams
	pars.beams = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{pars.imageSize[0], pars.imageSize[1]}});
	pars.beamsIndex = {}; //clear index
	{
		int beam_count = 1;
		for (auto y = 0; y < pars.qMask.get_dimj(); ++y)
		{
			for (auto x = 0; x < pars.qMask.get_dimi(); ++x)
			{
				if (mask.at(y, x) == 1)
				{
					pars.beamsIndex.push_back((size_t)y * pars.qMask.get_dimi() + (size_t)x);
					pars.beams.at(y, x) = beam_count++;
				}
			}
		}
	}
}

inline void setupBeams_HRTEM(Parameters<PRISMATIC_FLOAT_PRECISION> &pars)
{
	// determine which beams (AKA plane waves, or Fourier components) are relevant for the calculation

	Array1D<PRISMATIC_FLOAT_PRECISION> xv = makeFourierCoords(pars.imageSize[1],
															  (PRISMATIC_FLOAT_PRECISION)1 / pars.imageSize[1]);
	Array1D<PRISMATIC_FLOAT_PRECISION> yv = makeFourierCoords(pars.imageSize[0],
															  (PRISMATIC_FLOAT_PRECISION)1 / pars.imageSize[0]);
	pair<Array2D<PRISMATIC_FLOAT_PRECISION>, Array2D<PRISMATIC_FLOAT_PRECISION>> mesh_a = meshgrid(yv, xv);

	// create beam mask and count beams
	Prismatic::Array2D<unsigned int> mask;
	mask = zeros_ND<2, unsigned int>({{pars.imageSize[0], pars.imageSize[1]}});
	pars.numberBeams = 0;
	
	PRISMATIC_FLOAT_PRECISION minXstep = pars.lambda / pars.tiledCellDim[2];
	PRISMATIC_FLOAT_PRECISION minYstep = pars.lambda / pars.tiledCellDim[1];

	long interp_fx;
	long interp_fy;

	if(pars.meta.tiltMode == TiltSelection::Rectangular)
	{

		interp_fx = (pars.xTiltStep_tem >= minXstep) ? (long)round(pars.xTiltStep_tem / minXstep): 1; //use interpolation factor to control tilt step selection
		interp_fy = (pars.xTiltStep_tem >= minYstep) ? (long)round(pars.yTiltStep_tem / minYstep): 1; //use interpolation factor to control tilt step selection
	}
	else
	{
		interp_fx = pars.meta.interpolationFactorX;
		interp_fy = pars.meta.interpolationFactorY;
	}

	PRISMATIC_FLOAT_PRECISION relTiltX = 0.0;
	PRISMATIC_FLOAT_PRECISION relTiltY = 0.0;
	
	pars.xTilts_tem = {};
	pars.yTilts_tem = {};
	pars.xTiltsInd_tem = {};
	pars.yTiltsInd_tem = {};
	for (auto y = 0; y < pars.qMask.get_dimj(); ++y)
	{
		for (auto x = 0; x < pars.qMask.get_dimi(); ++x)
		{	//only get one beam
			relTiltX = std::abs(pars.qxa.at(y, x)*pars.lambda - pars.xTiltOffset_tem);
			relTiltY = std::abs(pars.qya.at(y, x)*pars.lambda - pars.yTiltOffset_tem);
			bool beamCheck;
			if(pars.meta.tiltMode == TiltSelection::Rectangular)
			{
				beamCheck = ((relTiltX <= pars.maxXtilt_tem and relTiltY <= pars.maxYtilt_tem) and 
							(relTiltX >= pars.minXtilt_tem or relTiltY >= pars.minYtilt_tem )) and
							(long)round(mesh_a.first.at(y, x)) % interp_fy == 0 and
							(long)round(mesh_a.second.at(y, x)) % interp_fx == 0 and 
							pars.qMask.at(y, x) == 1;
			}
			else
			{
				PRISMATIC_FLOAT_PRECISION cur_qr = sqrt(pow(relTiltX, 2) + pow(relTiltY,2));
				beamCheck = (cur_qr <= pars.meta.maxRtilt and cur_qr >= pars.meta.minRtilt) and
							(long)round(mesh_a.first.at(y, x)) % interp_fy == 0 and
							(long)round(mesh_a.second.at(y, x)) % interp_fx == 0 and 
							pars.qMask.at(y, x) == 1;
			}

			if (beamCheck)
			{
				mask.at(y, x) = 1;
				pars.xTilts_tem.push_back(pars.qxa.at(y, x)*pars.lambda);
				pars.yTilts_tem.push_back(pars.qya.at(y, x)*pars.lambda);
				pars.xTiltsInd_tem.push_back((int) round(mesh_a.second.at(y, x)) / interp_fx);
				pars.yTiltsInd_tem.push_back((int) round(mesh_a.first.at(y, x)) / interp_fy);
				++pars.numberBeams;
			}
		}
	}

	std::cout << "Number of total tilts: " << pars.numberBeams << std::endl;
	// number the beams
	pars.beams = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{pars.imageSize[0], pars.imageSize[1]}});
	pars.beamsIndex = {};
	{
		int beam_count = 1;
		for (auto y = 0; y < pars.qMask.get_dimj(); ++y)
		{
			for (auto x = 0; x < pars.qMask.get_dimi(); ++x)
			{
				if (mask.at(y, x) == 1)
				{
					pars.beamsIndex.push_back((size_t)y * pars.qMask.get_dimi() + (size_t)x);
					pars.beams.at(y, x) = beam_count++;
				}
			}
		}
	}
}

inline void setupSMatrixCoordinates(Parameters<PRISMATIC_FLOAT_PRECISION> &pars)
{

	// get the indices for the compact S-matrix
	pars.qxInd = zeros_ND<1, size_t>({{pars.imageSize[1] / 2}});
	pars.qyInd = zeros_ND<1, size_t>({{pars.imageSize[0] / 2}});
	{
		long n_0 = pars.imageSize[0];
		long n_1 = pars.imageSize[1];
		long n_quarter0 = pars.imageSize[0] / 4;
		long n_quarter1 = pars.imageSize[1] / 4;
		for (auto i = 0; i < n_quarter0; ++i)
		{
			pars.qyInd[i] = i;
			pars.qyInd[i + n_quarter0] = (i - n_quarter0) + n_0;
		}
		for (auto i = 0; i < n_quarter1; ++i)
		{
			pars.qxInd[i] = i;
			pars.qxInd[i + n_quarter1] = (i - n_quarter1) + n_1;
		}
	}
}

inline void downsampleFourierComponents(Parameters<PRISMATIC_FLOAT_PRECISION> &pars)
{
	// downsample Fourier components to only keep relevant/nonzero values
	pars.imageSizeOutput = pars.imageSize;
	pars.imageSizeOutput[0] /= 2;
	pars.imageSizeOutput[1] /= 2;
	pars.pixelSizeOutput = pars.pixelSize;
	pars.pixelSizeOutput[0] *= 2;
	pars.pixelSizeOutput[1] *= 2;


	pars.qxaOutput = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{pars.qyInd.size(), pars.qxInd.size()}});
	pars.qyaOutput = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{pars.qyInd.size(), pars.qxInd.size()}});
	pars.beamsOutput = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{pars.qyInd.size(), pars.qxInd.size()}});
	
	for (auto y = 0; y < pars.qyInd.size(); ++y)
	{
		for (auto x = 0; x < pars.qxInd.size(); ++x)
		{
			pars.qxaOutput.at(y, x) = pars.qxa.at(pars.qyInd[y], pars.qxInd[x]);
			pars.qyaOutput.at(y, x) = pars.qya.at(pars.qyInd[y], pars.qxInd[x]);
			pars.beamsOutput.at(y, x) = pars.beams.at(pars.qyInd[y], pars.qxInd[x]);
		}
	}
}

void propagatePlaneWave_CPU(Parameters<PRISMATIC_FLOAT_PRECISION> &pars,
							size_t currentBeam,
							Array2D<complex<PRISMATIC_FLOAT_PRECISION>> &psi,
							const PRISMATIC_FFTW_PLAN &plan_forward,
							const PRISMATIC_FFTW_PLAN &plan_inverse,
							mutex &fftw_plan_lock)
{
	// propagates a single plan wave and fills in the corresponding section of compact S-matrix, very similar to multislice

	psi[pars.beamsIndex[currentBeam]] = 1;
	const PRISMATIC_FLOAT_PRECISION slice_size = (PRISMATIC_FLOAT_PRECISION)psi.size();
	PRISMATIC_FFTW_EXECUTE(plan_inverse);
	for (auto &i : psi)
		i /= slice_size;													   // fftw scales by N, need to correct
	const complex<PRISMATIC_FLOAT_PRECISION> *trans_t = &pars.transmission[0]; // pointer to beginning of the transmission array
	for (auto a2 = 0; a2 < pars.numPlanes; ++a2)
	{
		for (auto &p : psi)
			p *= (*trans_t++);				  // transmit
		PRISMATIC_FFTW_EXECUTE(plan_forward); // FFT
		for (auto i = psi.begin(), j = pars.prop.begin(); i != psi.end(); ++i, ++j)
			*i *= (*j);						  // propagate
		PRISMATIC_FFTW_EXECUTE(plan_inverse); // IFFT
		for (auto &i : psi)
			i /= slice_size; // fftw scales by N, need to correct
	}
	PRISMATIC_FFTW_EXECUTE(plan_forward); // final FFT to get result at detector plane

	// only keep the necessary plane waves
	Array2D<complex<PRISMATIC_FLOAT_PRECISION>> psi_small = zeros_ND<2, complex<PRISMATIC_FLOAT_PRECISION>>(
		{{pars.qyInd.size(), pars.qxInd.size()}});

	unique_lock<mutex> gatekeeper(fftw_plan_lock);
	PRISMATIC_FFTW_PLAN plan_final = PRISMATIC_FFTW_PLAN_DFT_2D(psi_small.get_dimj(), psi_small.get_dimi(),
																reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_small[0]),
																reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_small[0]),
																FFTW_BACKWARD, FFTW_ESTIMATE);
	gatekeeper.unlock();
	for (auto y = 0; y < pars.qyInd.size(); ++y)
	{
		for (auto x = 0; x < pars.qxInd.size(); ++x)
		{
			psi_small.at(y, x) = psi.at(pars.qyInd[y], pars.qxInd[x]);
		}
	}

	// final FFT to get the cropped plane wave result in real space
	PRISMATIC_FFTW_EXECUTE(plan_final);
	gatekeeper.lock();
	PRISMATIC_FFTW_DESTROY_PLAN(plan_final);
	gatekeeper.unlock();

	// insert the cropped/propagated plane wave into the relevant slice of the compact S-matrix
	complex<PRISMATIC_FLOAT_PRECISION> *S_t = &pars.Scompact[currentBeam * pars.Scompact.get_dimj() * pars.Scompact.get_dimi()];
	const PRISMATIC_FLOAT_PRECISION N_small = (PRISMATIC_FLOAT_PRECISION)psi_small.size();
	for (auto &jj : psi_small)
	{
		*S_t++ = jj / N_small;
	}
}

void propagatePlaneWave_CPU_batch(Parameters<PRISMATIC_FLOAT_PRECISION> &pars,
								  size_t currentBeam,
								  size_t stopBeam,
								  Array1D<complex<PRISMATIC_FLOAT_PRECISION>> &psi_stack,
								  const PRISMATIC_FFTW_PLAN &plan_forward,
								  const PRISMATIC_FFTW_PLAN &plan_inverse,
								  mutex &fftw_plan_lock)
{
	// propagates a batch of plane waves and fills in the corresponding sections of compact S-matrix
	const size_t slice_size = pars.imageSize[0] * pars.imageSize[1];
	const PRISMATIC_FLOAT_PRECISION slice_size_f = (PRISMATIC_FLOAT_PRECISION)slice_size;
	{
		int beam_count = 0;
		for (auto jj = currentBeam; jj < stopBeam; ++jj)
		{
			psi_stack[beam_count * slice_size + pars.beamsIndex[jj]] = 1;
			++beam_count;
		}
	}

	PRISMATIC_FFTW_EXECUTE(plan_inverse);
	for (auto &i : psi_stack)
		i /= slice_size_f; // fftw scales by N, need to correct
	complex<PRISMATIC_FLOAT_PRECISION> *slice_ptr = &pars.transmission[0];
	for (auto a2 = 0; a2 < pars.numPlanes; ++a2)
	{
		// transmit each of the probes in the batch
		for (auto batch_idx = 0; batch_idx < min(pars.meta.batchSizeCPU, stopBeam - currentBeam); ++batch_idx)
		{
			auto t_ptr = slice_ptr; // start at the beginning of the current slice
			auto psi_ptr = &psi_stack[batch_idx * slice_size];
			for (auto jj = 0; jj < slice_size; ++jj)
			{
				*psi_ptr++ *= (*t_ptr++); // transmit
			}
		}
		slice_ptr += slice_size;			  // advance to point to the beginning of the next potential slice
		PRISMATIC_FFTW_EXECUTE(plan_forward); // FFT

		// propagate each of the probes in the batch
		for (auto batch_idx = 0; batch_idx < min(pars.meta.batchSizeCPU, stopBeam - currentBeam); ++batch_idx)
		{
			auto p_ptr = pars.prop.begin();
			auto psi_ptr = &psi_stack[batch_idx * slice_size];
			for (auto jj = 0; jj < slice_size; ++jj)
			{
				*psi_ptr++ *= (*p_ptr++); // propagate
			}
		}
		PRISMATIC_FFTW_EXECUTE(plan_inverse); // IFFT
		for (auto &i : psi_stack)
			i /= slice_size_f; // fftw scales by N, need to correct
	}
	PRISMATIC_FFTW_EXECUTE(plan_forward);

	// only keep the necessary plane waves

	if(pars.meta.algorithm == Algorithm::HRTEM) // center defocus at middle of cell if running HRTEM
	{
		// back propagate each of the probes in the batch
		for (auto batch_idx = 0; batch_idx < min(pars.meta.batchSizeCPU, stopBeam - currentBeam); ++batch_idx)
		{
			auto p_ptr = pars.propBack.begin();
			auto psi_ptr = &psi_stack[batch_idx * slice_size];
			for (auto jj = 0; jj < slice_size; ++jj)
			{
				*psi_ptr++ *= (*p_ptr++); // propagate
			}
		}
	}

	Array2D<complex<PRISMATIC_FLOAT_PRECISION>> psi_small = zeros_ND<2, complex<PRISMATIC_FLOAT_PRECISION>>(
		{{pars.qyInd.size(), pars.qxInd.size()}});
	const PRISMATIC_FLOAT_PRECISION N_small = (PRISMATIC_FLOAT_PRECISION)psi_small.size();
	unique_lock<mutex> gatekeeper(fftw_plan_lock);
	PRISMATIC_FFTW_PLAN plan_final = PRISMATIC_FFTW_PLAN_DFT_2D(psi_small.get_dimj(), psi_small.get_dimi(),
																reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_small[0]),
																reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_small[0]),
																FFTW_BACKWARD, FFTW_ESTIMATE);
	gatekeeper.unlock();
	int batch_idx = 0;
	while (currentBeam < stopBeam)
	{
		for (auto y = 0; y < pars.qyInd.size(); ++y)
		{
			for (auto x = 0; x < pars.qxInd.size(); ++x)
			{
				psi_small.at(y, x) = psi_stack[batch_idx * slice_size + pars.qyInd[y] * pars.imageSize[1] + pars.qxInd[x]];
			}
		}
		PRISMATIC_FFTW_EXECUTE(plan_final);
		complex<PRISMATIC_FLOAT_PRECISION> *S_t = &pars.Scompact[currentBeam * pars.Scompact.get_dimj() * pars.Scompact.get_dimi()];
		for (auto &jj : psi_small)
		{
			*S_t++ = jj / N_small;
		}
		++currentBeam;
		++batch_idx;
	}
	gatekeeper.lock();
	PRISMATIC_FFTW_DESTROY_PLAN(plan_final);
	gatekeeper.unlock();
}

void fill_Scompact_CPUOnly(Parameters<PRISMATIC_FLOAT_PRECISION> &pars)
{
	// populates the compact S-matrix using CPU resources

	extern mutex fftw_plan_lock; // lock for protecting FFTW plans

	// initialize arrays
	pars.Scompact = zeros_ND<3, complex<PRISMATIC_FLOAT_PRECISION>>(
		{{pars.numberBeams, pars.imageSize[0] / 2, pars.imageSize[1] / 2}});
	pars.transmission = zeros_ND<3, complex<PRISMATIC_FLOAT_PRECISION>>(
		{{pars.pot.get_dimk(), pars.pot.get_dimj(), pars.pot.get_dimi()}});
	{
		auto p = pars.pot.begin();
		for (auto &j : pars.transmission)
			j = exp(i * pars.sigma * (*p++));
	}

	// prepare to launch the calculation
	vector<thread> workers;
	workers.reserve(pars.meta.numThreads);												  // prevents multiple reallocations
	const size_t PRISMATIC_PRINT_FREQUENCY_BEAMS = max((size_t)1, pars.numberBeams / 10); // for printing status
	WorkDispatcher dispatcher(0, pars.numberBeams);
	pars.meta.batchSizeCPU = min(pars.meta.batchSizeTargetCPU, max((size_t)1, pars.numberBeams / pars.meta.numThreads));

	// initialize FFTW threads
	PRISMATIC_FFTW_INIT_THREADS();
	PRISMATIC_FFTW_PLAN_WITH_NTHREADS(pars.meta.numThreads);
	for (auto t = 0; t < pars.meta.numThreads; ++t)
	{
		cout << "Launching thread #" << t << " to compute beams\n";
		workers.push_back(thread([&pars, &dispatcher, &PRISMATIC_PRINT_FREQUENCY_BEAMS]() {
			// allocate array for psi just once per thread
			//				Array2D<complex<PRISMATIC_FLOAT_PRECISION> > psi = zeros_ND<2, complex<PRISMATIC_FLOAT_PRECISION> >(
			//						{{pars.imageSize[0], pars.imageSize[1]}});
			size_t currentBeam, stopBeam;
			currentBeam = stopBeam = 0;
			if (dispatcher.getWork(currentBeam, stopBeam, pars.meta.batchSizeCPU))
			{
				Array1D<complex<PRISMATIC_FLOAT_PRECISION>> psi_stack = zeros_ND<1, complex<PRISMATIC_FLOAT_PRECISION>>(
					{{pars.imageSize[0] * pars.imageSize[1] * pars.meta.batchSizeCPU}});
				//				PRISMATIC_FFTW_PLAN plan_forward = PRISMATIC_FFTW_PLAN_DFT_2D(psi.get_dimj(), psi.get_dimi(),
				//				                                                      reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi[0]),
				//				                                                      reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi[0]),
				//				                                                      FFTW_FORWARD, FFTW_MEASURE);
				//				PRISMATIC_FFTW_PLAN plan_inverse = PRISMATIC_FFTW_PLAN_DFT_2D(psi.get_dimj(), psi.get_dimi(),
				//				                                                      reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi[0]),
				//				                                                      reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi[0]),
				//				                                                      FFTW_BACKWARD, FFTW_MEASURE);

				// setup batch FFTW parameters
				const int rank = 2;
				int n[] = {(int)pars.imageSize[0], (int)pars.imageSize[1]};
				const int howmany = pars.meta.batchSizeCPU;
				int idist = n[0] * n[1];
				int odist = n[0] * n[1];
				int istride = 1;
				int ostride = 1;
				int *inembed = n;
				int *onembed = n;

				unique_lock<mutex> gatekeeper(fftw_plan_lock);
				PRISMATIC_FFTW_PLAN plan_forward = PRISMATIC_FFTW_PLAN_DFT_BATCH(rank, n, howmany,
																				 reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_stack[0]),
																				 inembed,
																				 istride, idist,
																				 reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_stack[0]),
																				 onembed,
																				 ostride, odist,
																				 FFTW_FORWARD, FFTW_MEASURE);
				PRISMATIC_FFTW_PLAN plan_inverse = PRISMATIC_FFTW_PLAN_DFT_BATCH(rank, n, howmany,
																				 reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_stack[0]),
																				 inembed,
																				 istride, idist,
																				 reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_stack[0]),
																				 onembed,
																				 ostride, odist,
																				 FFTW_BACKWARD, FFTW_MEASURE);
				gatekeeper.unlock(); // unlock it so we only block as long as necessary to deal with plans

				// main work loop
				do
				{ // synchronously get work assignment
					while (currentBeam < stopBeam)
					{
						if (currentBeam % PRISMATIC_PRINT_FREQUENCY_BEAMS < pars.meta.batchSizeCPU |
							currentBeam == 100)
						{
							cout << "Computing Plane Wave #" << currentBeam << "/" << pars.numberBeams << endl;
						}

						// re-zero psi each iteration
						memset((void *)&psi_stack[0], 0,
							   psi_stack.size() * sizeof(complex<PRISMATIC_FLOAT_PRECISION>));
						//							propagatePlaneWave_CPU(pars, currentBeam, psi, plan_forward, plan_inverse, fftw_plan_lock);
						propagatePlaneWave_CPU_batch(pars, currentBeam, stopBeam, psi_stack, plan_forward,
													 plan_inverse, fftw_plan_lock);
#ifdef PRISMATIC_BUILDING_GUI
						pars.progressbar->signalScompactUpdate(currentBeam, pars.numberBeams);
#endif
						currentBeam = stopBeam;
					}
				} while (dispatcher.getWork(currentBeam, stopBeam, pars.meta.batchSizeCPU));

				// clean up plans
				gatekeeper.lock();
				PRISMATIC_FFTW_DESTROY_PLAN(plan_forward);
				PRISMATIC_FFTW_DESTROY_PLAN(plan_inverse);
				gatekeeper.unlock();
			}
		}));
	}
	cout << "Waiting for threads...\n";
	for (auto &t : workers)
		t.join();
	PRISMATIC_FFTW_CLEANUP_THREADS();
#ifdef PRISMATIC_BUILDING_GUI
	pars.progressbar->setProgress(100);
	pars.progressbar->signalCalcStatusMessage(QString("Plane Wave ") +
											  QString::number(pars.numberBeams) +
											  QString("/") +
											  QString::number(pars.numberBeams));
#endif //PRISMATIC_BUILDING_GUI
}

void PRISM02_calcSMatrix(Parameters<PRISMATIC_FLOAT_PRECISION> &pars)
{
	// propagate plane waves to construct compact S-matrix

	cout << "Entering PRISM02_calcSMatrix" << endl;

	// setup some coordinates
	setupCoordinates(pars);

	// setup the beams and their indices
	if(pars.meta.algorithm == Algorithm::PRISM)
	{
		setupBeams(pars);
	}
	else if(pars.meta.algorithm == Algorithm::HRTEM)
	{
		setupBeams_HRTEM(pars);
	}

	// setup coordinates for nonzero values of compact S-matrix
	setupSMatrixCoordinates(pars);

	cout << "Computing compact S matrix" << endl;

#ifdef PRISMATIC_BUILDING_GUI
	pars.progressbar->signalDescriptionMessage("Computing compact S-matrix");
	pars.progressbar->signalScompactUpdate(-1, pars.numberBeams);
#endif //PRISMATIC_BUILDING_GUI

	// populate compact S-matrix
	fill_Scompact(pars);

	// only keep the relevant/nonzero Fourier components
	downsampleFourierComponents(pars);

	if(pars.meta.saveSMatrix)
	{
		std::cout << "Writing scattering matrix to output file." << std::endl;
		setupSMatrixOutput(pars, pars.fpFlag);
		H5::Group smatrix_group = pars.outputFile.openGroup("4DSTEM_simulation/data/realslices/smatrix_fp" + getDigitString(pars.fpFlag));
		hsize_t mdims[3] = {pars.Scompact.get_dimi(), pars.Scompact.get_dimj(), pars.numberBeams};
		std::vector<size_t> order = {0, 1, 2};
		
		std::cout << pars.Scompact.at(0,3,5).real() << std::endl;
		std::cout << pars.Scompact.at(0,3,5).imag() << std::endl;
		writeComplexDataSet(smatrix_group, "realslice", &pars.Scompact[0], mdims, 3, order);
	}
}

void PRISM02_importSMatrix(Parameters<PRISMATIC_FLOAT_PRECISION> &pars)
{
	std::cout << "Setting up auxilary variables according to " << pars.meta.importFile << " metadata." << std::endl;
	//scope out imported smatrix as soon as possible
	{
		Array3D<std::complex<PRISMATIC_FLOAT_PRECISION>> inSMatrix;
		std::vector<size_t> order ={0,1,2};

		if(pars.meta.importPath.size() > 0)
		{
			readComplexDataSet(pars.Scompact, pars.meta.importFile, pars.meta.importPath, order);
		}
		else //read default path
		{
			std::string groupPath = "4DSTEM_simulation/data/realslices/smatrix_fp" + getDigitString(pars.fpFlag) + "/realslice";
			readComplexDataSet(pars.Scompact, pars.meta.importFile, groupPath, order);
		}
	}
	
	//acquire necessary metadata to create auxillary variables
	std::string groupPath = "4DSTEM_simulation/metadata/metadata_0/original/simulation_parameters";
	PRISMATIC_FLOAT_PRECISION meta_cellDims[3];
	int meta_fx;
	int meta_fy;
	readAttribute(pars.meta.importFile, groupPath, "c", meta_cellDims);
	readAttribute(pars.meta.importFile, groupPath, "fx", meta_fx);
	readAttribute(pars.meta.importFile, groupPath, "fy", meta_fy);

	PRISMATIC_FLOAT_PRECISION meta_rpixel[2];
	PRISMATIC_FLOAT_PRECISION tmp_rpixel;
	readAttribute(pars.meta.importFile, groupPath, "py", tmp_rpixel);
	meta_rpixel[0] = tmp_rpixel;
	readAttribute(pars.meta.importFile, groupPath, "px", tmp_rpixel);
	meta_rpixel[1] = tmp_rpixel;

	pars.tiledCellDim[0] = meta_cellDims[0];
	pars.tiledCellDim[1] = meta_cellDims[1];
	pars.tiledCellDim[2] = meta_cellDims[2];
	pars.meta.realspacePixelSize[0] = meta_rpixel[0];
	pars.meta.realspacePixelSize[1] = meta_rpixel[1];
	pars.meta.interpolationFactorX = meta_fx;
	pars.meta.interpolationFactorY = meta_fy;

	std::vector<PRISMATIC_FLOAT_PRECISION> pixelSize{(PRISMATIC_FLOAT_PRECISION) pars.tiledCellDim[1], (PRISMATIC_FLOAT_PRECISION) pars.tiledCellDim[2]};

	pars.imageSize[0] = pars.Scompact.get_dimj()*2;
	pars.imageSize[1] = pars.Scompact.get_dimi()*2;
	pixelSize[0] /= pars.imageSize[0];
	pixelSize[1] /= pars.imageSize[1];

	Array1D<PRISMATIC_FLOAT_PRECISION> qx = makeFourierCoords(pars.imageSize[1], pars.pixelSize[1]);
	Array1D<PRISMATIC_FLOAT_PRECISION> qy = makeFourierCoords(pars.imageSize[0], pars.pixelSize[0]);
	pair<Array2D<PRISMATIC_FLOAT_PRECISION>, Array2D<PRISMATIC_FLOAT_PRECISION>> mesh = meshgrid(qy, qx);
	pars.qya = mesh.first;
	pars.qxa = mesh.second;
	Array2D<PRISMATIC_FLOAT_PRECISION> q2(pars.qya);
	transform(pars.qxa.begin(), pars.qxa.end(),
			  pars.qya.begin(), q2.begin(), [](const PRISMATIC_FLOAT_PRECISION &a, const PRISMATIC_FLOAT_PRECISION &b) {
				  return a * a + b * b;
			  });
	pars.q2 = q2;
	
	// get qMax
	long long ncx = (long long)floor((PRISMATIC_FLOAT_PRECISION)pars.imageSize[1] / 2);
	PRISMATIC_FLOAT_PRECISION dpx = 1.0 / ((PRISMATIC_FLOAT_PRECISION)pars.imageSize[1] * pars.meta.realspacePixelSize[1]);
	long long ncy = (long long)floor((PRISMATIC_FLOAT_PRECISION)pars.imageSize[0] / 2);
	PRISMATIC_FLOAT_PRECISION dpy = 1.0 / ((PRISMATIC_FLOAT_PRECISION)pars.imageSize[0] * pars.meta.realspacePixelSize[0]);
	pars.qMax = std::min(dpx * (ncx), dpy * (ncy)) / 2;

	// construct anti-aliasing mask
	pars.qMask = zeros_ND<2, unsigned int>({{pars.imageSize[0], pars.imageSize[1]}});
	{
		long offset_x = pars.qMask.get_dimi() / 4;
		long offset_y = pars.qMask.get_dimj() / 4;
		long ndimy = (long)pars.qMask.get_dimj();
		long ndimx = (long)pars.qMask.get_dimi();
		for (long y = 0; y < pars.qMask.get_dimj() / 2; ++y)
		{
			for (long x = 0; x < pars.qMask.get_dimi() / 2; ++x)
			{
				pars.qMask.at(((y - offset_y) % ndimy + ndimy) % ndimy,
							  ((x - offset_x) % ndimx + ndimx) % ndimx) = 1;
			}
		}
	}

	setupBeams(pars);
	setupSMatrixCoordinates(pars);
	downsampleFourierComponents(pars);

	if(pars.meta.saveSMatrix)
	{
		std::cout << "Writing scattering matrix to output file." << std::endl;
		setupSMatrixOutput(pars, pars.fpFlag);
		H5::Group smatrix_group = pars.outputFile.openGroup("4DSTEM_simulation/data/realslices/smatrix_fp" + getDigitString(pars.fpFlag));
		hsize_t mdims[3] = {pars.Scompact.get_dimi(), pars.Scompact.get_dimj(), pars.numberBeams};

		std::array<size_t, 3> dims_in = {pars.Scompact.get_dimi(), pars.Scompact.get_dimj(), pars.Scompact.get_dimk()};
		std::vector<size_t> order = {0, 1, 2};
		writeComplexDataSet(smatrix_group, "realslice", &pars.Scompact[0], mdims, 3, order);
	}

}

} // namespace Prismatic
