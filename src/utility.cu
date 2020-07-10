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

#include "utility.cuh"
#include "utility.h"
#include "cuComplex.h"
#include <iostream>
#include <sstream>
#include <mutex>

// std::mutex HDF5_lock;

#define PI 3.14159265359
// define some constants
__device__ __constant__ float pi_f                  = PI;
__device__ __constant__ cuFloatComplex i_f          = {0, 1};
__device__ __constant__ cuFloatComplex pi_cx_f      = {PI, 0};
__device__ __constant__ cuFloatComplex minus_2pii_f = {0, -2*PI};
__device__ __constant__ double pi                   = PI;
__device__ __constant__ cuDoubleComplex i           = {0, 1};
__device__ __constant__ cuDoubleComplex pi_cx       = {PI, 0};
__device__ __constant__ cuDoubleComplex minus_2pii  = {0, -2*PI};

//atomicAdd for doubles on devices with compute capability < 6. This is directly copied from the CUDA Programming Guide
#if __CUDA_ARCH__ < 600
__device__  double atomicAdd_double(double* address, const double val)
{
	unsigned long long int* address_as_ull =
			(unsigned long long int*)address;
	unsigned long long int old = *address_as_ull, assumed;

	do {
		assumed = old;
		old = atomicCAS(address_as_ull, assumed,
		                __double_as_longlong(val +
		                                     __longlong_as_double(assumed)));

//		 Note: uses integer comparison to avoid hang in case of NaN (since NaN != NaN)
	} while (assumed != old);

	return __longlong_as_double(old);
}
#endif



// computes exp(real(a) + i * imag(a))
__device__ __forceinline__ cuDoubleComplex exp_cx(const cuDoubleComplex a){
	double e = exp(a.x);
	double s,c;
	sincos(a.y, &s, &c);
	return make_cuDoubleComplex(e*c, e*s);
}
__device__ __forceinline__ cuFloatComplex exp_cx(const cuFloatComplex a){
	float e = expf(a.x);
	float s,c;
	sincosf(a.y, &s, &c);
	return make_cuFloatComplex(e*c, e*s);
}

__global__ void initializePsi_oneNonzero(cuFloatComplex *psi_d, const size_t N, const size_t beamLoc){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		psi_d[idx] = (idx == beamLoc) ? make_cuFloatComplex(1,0):make_cuFloatComplex(0,0);
	}
}

__global__ void initializePsi_oneNonzero(cuDoubleComplex *psi_d, const size_t N, const size_t beamLoc){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		psi_d[idx] = (idx == beamLoc) ? make_cuDoubleComplex(1,0):make_cuDoubleComplex(0,0);
	}
}

// multiply two complex arrays
__global__ void multiply_inplace(cuDoubleComplex* arr,
                                 const cuDoubleComplex* other,
                                 const size_t N){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		cuDoubleComplex a = arr[idx];
		cuDoubleComplex o = other[idx];
		arr[idx].x = a.x * o.x - a.y * o.y;
		arr[idx].y = a.x * o.y + a.y * o.x;
	}
}

// multiply two complex arrays
__global__ void multiply_inplace(cuFloatComplex* arr,
                                 const cuFloatComplex* other,
                                 const size_t N){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		cuFloatComplex a = arr[idx];
		cuFloatComplex o = other[idx];
		arr[idx].x = a.x * o.x - a.y * o.y;
		arr[idx].y = a.x * o.y + a.y * o.x;
	}
}

// multiply two complex arrays
__global__ void multiply_cx(cuDoubleComplex* arr,
                             const cuDoubleComplex* other,
                             const size_t N){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
//		cuDoubleComplex a = arr[idx];
//		cuDoubleComplex o = other[idx];
		arr[idx] = cuCmul(arr[idx], other[idx]);
	}
}

// multiply two complex arrays
__global__ void multiply_cx(cuFloatComplex* arr,
                            const cuFloatComplex* other,
                            const size_t N){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		arr[idx] = cuCmulf(arr[idx], other[idx]);
	}
}

// multiply complex array by scalar
__global__ void multiply_cxarr_scalar(cuDoubleComplex* arr,
                                      const cuDoubleComplex val,
                                      const size_t N){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		arr[idx] = cuCmul(arr[idx], val);
	}
}

// multiply complex array by scalar
__global__ void multiply_cxarr_scalar(cuFloatComplex* arr,
                                      const cuFloatComplex val,
                                      const size_t N){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		arr[idx] = cuCmulf(arr[idx], val);
	}
}

// multiply array by scalar
__global__ void multiply_arr_scalar(double* arr,
                                    const double val,
                                    const size_t N){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		arr[idx] = arr[idx]*val;
	}
}

// multiply array by scalar
__global__ void multiply_arr_scalar(float* arr,
                                    const float val,
                                    const size_t N){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		arr[idx] = arr[idx]*val;
	}
}


//// divide two complex arrays
//__global__ void divide_inplace(PRISMATIC_CUDA_COMPLEX_FLOAT* arr,
//                               const PRISMATIC_FLOAT_PRECISION val,
//                               const size_t N){
//	int idx = threadIdx.x + blockDim.x*blockIdx.x;
//	if (idx < N) {
//		arr[idx].x /= val;
//		arr[idx].y /= val;
//	}
//}

__global__ void divide_inplace(cuDoubleComplex* arr,
                               const cuDoubleComplex val,
                               const size_t N){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		arr[idx] = cuCdiv(arr[idx], val);
	}
}

__global__ void divide_inplace(cuFloatComplex* arr,
                               const cuFloatComplex val,
                               const size_t N){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		arr[idx] = cuCdivf(arr[idx], val);
	}
}

// set all array values to val
__global__ void setAll(double *data, double val, size_t N) {
	int idx = threadIdx.x + blockDim.x * blockIdx.x;
	if (idx<N) {
		data[idx] = val;
	}
}

// set all array values to val
__global__ void setAll(float *data, float val, size_t N) {
	int idx = threadIdx.x + blockDim.x * blockIdx.x;
	if (idx<N) {
		data[idx] = val;
	}
}

// creates initial probe using existing GPU memory rather than streaming each probe
__global__ void initializePsi(cuDoubleComplex *psi_d,
                              const cuDoubleComplex* PsiProbeInit_d,
                              const double* qya_d,
                              const double* qxa_d,
                              const size_t N,
                              const double yp,
                              const double xp){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		cuDoubleComplex arg;
		arg = make_cuDoubleComplex(qxa_d[idx]*xp + qya_d[idx]*yp, 0);
		psi_d[idx] = cuCmul(PsiProbeInit_d[idx], exp_cx(cuCmul(minus_2pii,arg)));
	}
}

// creates initial probe using existing GPU memory rather than streaming each probe
__global__ void initializePsi(cuFloatComplex *psi_d,
                              const cuFloatComplex* PsiProbeInit_d,
                              const float* qya_d,
                              const float* qxa_d,
                              const size_t N,
                              const float yp,
                              const float xp){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		cuFloatComplex arg;
		arg = make_cuFloatComplex(qxa_d[idx]*xp + qya_d[idx]*yp, 0);
		psi_d[idx] = cuCmulf(PsiProbeInit_d[idx], exp_cx(cuCmulf(minus_2pii_f,arg)));
	}
}


// compute modulus squared of other and store in arr
__global__ void abs_squared(double* arr,
                            const cuDoubleComplex* other,
                            const size_t N){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		double re = other[idx].x;
		double im = other[idx].y;
		arr[idx] = re*re + im*im;
	}
}

// compute modulus squared of other and store in arr
__global__ void abs_squared(float* arr,
                            const cuFloatComplex* other,
                            const size_t N){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < N) {
		float re = other[idx].x;
		float im = other[idx].y;
		arr[idx] = re*re + im*im;
	}
}

__global__ void array_subset(const cuDoubleComplex* psi_d,
                             cuDoubleComplex* psi_small_d,
                             const size_t* qyInd_d,
                             const size_t* qxInd_d,
                             const size_t dimi,
                             const size_t dimj_small,
                             const size_t dimi_small){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < dimj_small*dimi_small) {
		int y = idx / (int)dimi_small;
		int x = idx % (int)dimi_small;
		int idxBig = qyInd_d[y] * dimi + qxInd_d[x];
		psi_small_d[idx] = psi_d[idxBig];
//		psi_small_d[idx] = make_cuFloatComplex(idx,idxBig);
	}
}
__global__ void array_subset(const cuFloatComplex* psi_d,
                             cuFloatComplex* psi_small_d,
                             const size_t* qyInd_d,
                             const size_t* qxInd_d,
                             const size_t dimi,
                             const size_t dimj_small,
                             const size_t dimi_small) {
	int idx = threadIdx.x + blockDim.x * blockIdx.x;
	if (idx < dimj_small * dimi_small) {
		int y = idx / (int) dimi_small;
		int x = idx % (int) dimi_small;
		int idxBig = qyInd_d[y] * dimi + qxInd_d[x];
		psi_small_d[idx] = psi_d[idxBig];
//		psi_small_d[idx] = make_cuFloatComplex(idx,idxBig);
	}
}




__global__ void shiftIndices(long* vec_out, const long by, const long imageSize, const long N){

		//int idx = threadIdx.x + blockDim.x * blockIdx.x;
		long idx = threadIdx.x + blockDim.x * blockIdx.x;
		if (idx < N){
			vec_out[idx] = (imageSize + ((idx - N/2 + by) % imageSize)) % imageSize;
//			vec_out[idx] =  (idx - N/2 + by) % imageSize;
//			vec_out[idx] = 0;
		}
	}

__global__ void zeroIndices(long* vec_out, const long N){

	int idx = threadIdx.x + blockDim.x * blockIdx.x;
	if (idx < N){
		vec_out[idx] = vec_out[idx] - vec_out[0];
	}
}

__global__ void resetIndices(long* vec_out, const long N){

	int idx = threadIdx.x + blockDim.x * blockIdx.x;
	if (idx < N){
		vec_out[idx] = idx;
	}
}


__global__ void computePhaseCoeffs(cuFloatComplex* phaseCoeffs,
                                   const cuFloatComplex *PsiProbeInit_d,
                                   const float * qyaReduce_d,
                                   const float * qxaReduce_d,
                                   const size_t *yBeams_d,
                                   const size_t *xBeams_d,
                                   const float yp,
                                   const float xp,
                                   const float yTiltShift,
                                   const float xTiltShift,
                                   const size_t dimi,
                                   const size_t numBeams){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < numBeams) {
		size_t yB = yBeams_d[idx];
		size_t xB = xBeams_d[idx];
		cuFloatComplex xp_cx = make_cuFloatComplex(xp, 0);
		cuFloatComplex yp_cx = make_cuFloatComplex(yp, 0);
		cuFloatComplex xTiltShift_cx = make_cuFloatComplex(xTiltShift, 0);
		cuFloatComplex yTiltShift_cx = make_cuFloatComplex(yTiltShift, 0);
		cuFloatComplex qya = make_cuFloatComplex(qyaReduce_d[yB * dimi + xB], 0);
		cuFloatComplex qxa = make_cuFloatComplex(qxaReduce_d[yB * dimi + xB], 0);
		cuFloatComplex arg1 = cuCmulf(qxa, cuCaddf(xp_cx, xTiltShift_cx));
		cuFloatComplex arg2 = cuCmulf(qya, cuCaddf(yp_cx, yTiltShift_cx));
		cuFloatComplex arg = cuCaddf(arg1, arg2);
		cuFloatComplex phase_shift = exp_cx(cuCmulf(minus_2pii_f, arg));
		phaseCoeffs[idx] = cuCmulf(phase_shift, PsiProbeInit_d[yB * dimi + xB]);
	}
}

__global__ void computePhaseCoeffs(cuDoubleComplex* phaseCoeffs,
                                   const cuDoubleComplex *PsiProbeInit_d,
                                   const double * qyaReduce_d,
                                   const double * qxaReduce_d,
                                   const size_t *yBeams_d,
                                   const size_t *xBeams_d,
                                   const double yp,
                                   const double xp,
                                   const double yTiltShift,
                                   const double xTiltShift,
                                   const size_t dimi,
                                   const size_t numBeams){
	int idx = threadIdx.x + blockDim.x*blockIdx.x;
	if (idx < numBeams) {
		size_t yB = yBeams_d[idx];
		size_t xB = xBeams_d[idx];
		cuDoubleComplex xp_cx = make_cuDoubleComplex(xp, 0);
		cuDoubleComplex yp_cx = make_cuDoubleComplex(yp, 0);
		cuDoubleComplex xTiltShift_cx = make_cuDoubleComplex(xTiltShift, 0);
		cuDoubleComplex yTiltShift_cx = make_cuDoubleComplex(yTiltShift, 0);
		cuDoubleComplex qya = make_cuDoubleComplex(qyaReduce_d[yB * dimi + xB], 0);
		cuDoubleComplex qxa = make_cuDoubleComplex(qxaReduce_d[yB * dimi + xB], 0);
		cuDoubleComplex arg1 = cuCmul(qxa, cuCadd(xp_cx, xTiltShift_cx));
		cuDoubleComplex arg2 = cuCmul(qya, cuCadd(yp_cx, yTiltShift_cx));
		cuDoubleComplex arg = cuCadd(arg1, arg2);
		cuDoubleComplex phase_shift = exp_cx(cuCmul(minus_2pii, arg));
		phaseCoeffs[idx] = cuCmul(phase_shift, PsiProbeInit_d[yB * dimi + xB]);
	}
}





// integrate computed intensities radially
__global__ void integrateDetector(const float* psiIntensity_ds,
                                  const float* alphaInd_d,
                                  float* integratedOutput,
                                  const size_t N,
                                  const size_t num_integration_bins) {
	int idx = threadIdx.x + blockDim.x * blockIdx.x;
	if (idx < N) {
		size_t alpha = (size_t)alphaInd_d[idx];
		if (alpha <= num_integration_bins)
			//atomicAdd(&integratedOutput[alpha-1], psiIntensity_ds[idx]);
			atomicAdd(&integratedOutput[alpha-1], psiIntensity_ds[idx]);
	}
}

__global__ void integrateDetector(const double* psiIntensity_ds,
                                  const double* alphaInd_d,
                                  double* integratedOutput,
                                  const size_t N,
                                  const size_t num_integration_bins) {
	int idx = threadIdx.x + blockDim.x * blockIdx.x;
	if (idx < N) {
		size_t alpha = (size_t)alphaInd_d[idx];
		if (alpha <= num_integration_bins)
			//atomicAdd(&integratedOutput[alpha-1], psiIntensity_ds[idx]);
			atomicAdd_double(&integratedOutput[alpha-1], psiIntensity_ds[idx]);
	}
}

__global__ void DPC_numerator_reduce(const float* psiIntensity_ds,
									 const float* q_coord,
									 float* numerator,
									 const size_t N){
	int idx = threadIdx.x + blockDim.x * blockIdx.x;
	if (idx < N){
		atomicAdd(&numerator[0], psiIntensity_ds[idx]*q_coord[idx]);
	}
}

__global__ void DPC_numerator_reduce(const double* psiIntensity_ds,
 									 const double* q_coord,
									 double* numerator,
									 const size_t N){
	int idx = threadIdx.x + blockDim.x * blockIdx.x;
		if (idx < N){
			atomicAdd_double(&numerator[0], psiIntensity_ds[idx]*q_coord[idx]);
		}
}

__global__ void DPC_denominator_reduce(const float* psiIntensity_ds,
									   float* denominator,
									   const size_t N){
	int idx = threadIdx.x + blockDim.x * blockIdx.x;
	if (idx < N){
		atomicAdd(&denominator[0], psiIntensity_ds[idx]);
	}										   
}

__global__ void DPC_denominator_reduce(const double* psiIntensity_ds,
									   double* denominator,
									   const size_t N){
	int idx = threadIdx.x + blockDim.x * blockIdx.x;
	if (idx < N){
		atomicAdd_double(&denominator[0], psiIntensity_ds[idx]);
	}			
}

void formatOutput_GPU_integrate(Prismatic::Parameters<PRISMATIC_FLOAT_PRECISION> &pars,
                                PRISMATIC_FLOAT_PRECISION *psiIntensity_ds,
                                const PRISMATIC_FLOAT_PRECISION *alphaInd_d,
                                PRISMATIC_FLOAT_PRECISION *output_ph,
								PRISMATIC_FLOAT_PRECISION *integratedOutput_ds,
								const PRISMATIC_FLOAT_PRECISION* qya_d,
								const PRISMATIC_FLOAT_PRECISION* qxa_d,
								const size_t currentSlice,
                                const size_t ay,
                                const size_t ax,
                                const size_t& dimj,
                                const size_t& dimi,
                                const cudaStream_t& stream,
                                const long& scale) {

	//save 4D output if applicable
    if (pars.meta.save4DOutput)
    {
		// This section could be improved. It currently makes a new 2D array, copies to it, and
		// then saves the image. This allocates arrays multiple times unneccessarily, and the allocated
		// memory isn't pinned, so the memcpy is not asynchronous.
		//std::string section4DFilename = generateFilename(pars, currentSlice, ay, ax);
		
		Prismatic::Array2D<PRISMATIC_FLOAT_PRECISION> currentImage = Prismatic::zeros_ND<2, PRISMATIC_FLOAT_PRECISION>(
				{{pars.psiProbeInit.get_dimj(), pars.psiProbeInit.get_dimi()}});
		cudaErrchk(cudaMemcpyAsync(&currentImage[0],
		                           psiIntensity_ds,
		                           pars.psiProbeInit.size() * sizeof(PRISMATIC_FLOAT_PRECISION),
		                           cudaMemcpyDeviceToHost,
								   stream));
								   
		//Need to scale the output by the square of the PRISM interpolation factor 
		// std::unique_lock<std::mutex> HDF5_gatekeeper(Prismatic::HDF5_lock);

		currentImage *= pars.scale;
		std::stringstream nameString;
		nameString << "/4DSTEM_simulation/data/datacubes/CBED_array_depth" << Prismatic::getDigitString(currentSlice);
		
		// H5::Group dataGroup = pars.outputFile.openGroup(nameString.str());
		// H5::DataSet CBED_data = dataGroup.openDataSet("datacube");

		hsize_t offset[4] = {ax,ay,0,0}; //order by ax, ay so that aligns with py4DSTEM
        PRISMATIC_FLOAT_PRECISION numFP = pars.meta.numFP;
        
        if(pars.meta.crop4DOutput)
        {
            Prismatic::Array2D<PRISMATIC_FLOAT_PRECISION> finalImage = cropOutput(currentImage,pars);
            hsize_t mdims[4] = {1,1,finalImage.get_dimi(),finalImage.get_dimj()};
            Prismatic::writeDatacube4D(pars, &finalImage[0],mdims,offset,numFP,nameString.str());
        }
        else
        {

            if (pars.meta.algorithm == Prismatic::Algorithm::Multislice){
                Prismatic::Array2D<PRISMATIC_FLOAT_PRECISION> finalImage = Prismatic::zeros_ND<2, PRISMATIC_FLOAT_PRECISION>(
                    {{pars.psiProbeInit.get_dimj()/2,pars.psiProbeInit.get_dimi()/2}});
                    {
                        long offset_x = pars.psiProbeInit.get_dimi() / 4;
                        long offset_y = pars.psiProbeInit.get_dimj() / 4;
                        long ndimy = (long) pars.psiProbeInit.get_dimj();
                        long ndimx = (long) pars.psiProbeInit.get_dimi();
                        for (long y = 0; y < pars.psiProbeInit.get_dimj() / 2; ++y) {
                            for (long x = 0; x < pars.psiProbeInit.get_dimi() / 2; ++x) {
                                finalImage.at(y, x) = currentImage.at(((y - offset_y) % ndimy + ndimy) % ndimy,
                                ((x - offset_x) % ndimx + ndimx) % ndimx);
                            }
                        }
                    }
                    
                    //finalImage = fftshift2(finalImage);
                    hsize_t mdims[4] = {1,1,pars.psiProbeInit.get_dimi()/2,pars.psiProbeInit.get_dimj()/2};
                    Prismatic::writeDatacube4D(pars, &finalImage[0],mdims,offset,numFP,nameString.str());
                    //finalImage.toMRC_f(section4DFilename.c_str());
                }else{                     
                    currentImage = fftshift2(currentImage);
                    hsize_t mdims[4] = {1,1,pars.psiProbeInit.get_dimi(),pars.psiProbeInit.get_dimj()};
                    Prismatic::writeDatacube4D(pars, &currentImage[0],mdims,offset,numFP,nameString.str());
                    //currentImage.toMRC_f(section4DFilename.c_str());
                }
        }
        // CBED_data.close();
        // dataGroup.close();
        // HDF5_gatekeeper.unlock();
    }
//		cudaSetDeviceFlags(cudaDeviceBlockingSync);


	size_t num_integration_bins = pars.detectorAngles.size();
	setAll <<< (num_integration_bins - 1) / BLOCK_SIZE1D + 1, BLOCK_SIZE1D, 0, stream >>>
	                                                                            (integratedOutput_ds, 0, num_integration_bins);

	integrateDetector <<< (dimj * dimi - 1) / BLOCK_SIZE1D + 1, BLOCK_SIZE1D, 0, stream >>>
	                                                                              (psiIntensity_ds, alphaInd_d, integratedOutput_ds,
			                                                                              dimj *
			                                                                              dimi, num_integration_bins);

	multiply_arr_scalar <<< (dimj * dimi - 1) / BLOCK_SIZE1D + 1, BLOCK_SIZE1D, 0, stream >>>
	                                                                                (integratedOutput_ds, scale, num_integration_bins);

	cudaErrchk(cudaMemcpyAsync(output_ph, integratedOutput_ds,
	                           num_integration_bins * sizeof(PRISMATIC_FLOAT_PRECISION),
	                           cudaMemcpyDeviceToHost, stream));

	//	 wait for the copy to complete and then copy on the host. Other host threads exist doing work so this wait isn't costing anything
	cudaErrchk(cudaStreamSynchronize(stream));
	const size_t stack_start_offset =
			currentSlice * pars.output.get_dimk() * pars.output.get_dimj() * pars.output.get_dimi() + ay * pars.output.get_dimj() * pars.output.get_dimi() + ax * pars.output.get_dimi();
	memcpy(&pars.output[stack_start_offset], output_ph, num_integration_bins * sizeof(PRISMATIC_FLOAT_PRECISION));
	
    if(pars.meta.saveDPC_CoM)
    {
		//device variables
		PRISMATIC_FLOAT_PRECISION *num_qx_d;
		PRISMATIC_FLOAT_PRECISION *num_qy_d;
		PRISMATIC_FLOAT_PRECISION *denominator_d;
		cudaErrchk(cudaMallocManaged(&num_qx_d, 1*sizeof(PRISMATIC_FLOAT_PRECISION)));
		cudaErrchk(cudaMallocManaged(&num_qy_d, 1*sizeof(PRISMATIC_FLOAT_PRECISION)));
		cudaErrchk(cudaMallocManaged(&denominator_d, 1*sizeof(PRISMATIC_FLOAT_PRECISION)));

		//host variables
		PRISMATIC_FLOAT_PRECISION *num_qx_h = new PRISMATIC_FLOAT_PRECISION[1];
		PRISMATIC_FLOAT_PRECISION *num_qy_h = new PRISMATIC_FLOAT_PRECISION[1];
		PRISMATIC_FLOAT_PRECISION *denominator_h = new PRISMATIC_FLOAT_PRECISION[1];
		num_qx_h[0] = 0.0;
		num_qy_h[0] = 0.0;
		denominator_h[0] = 0.0;

		//initialize device variables
		cudaErrchk(cudaMemcpyAsync(num_qx_d,&num_qx_h[0],1*sizeof(PRISMATIC_FLOAT_PRECISION),cudaMemcpyHostToDevice));
		cudaErrchk(cudaMemcpyAsync(num_qy_d,&num_qy_h[0],1*sizeof(PRISMATIC_FLOAT_PRECISION),cudaMemcpyHostToDevice));
		cudaErrchk(cudaMemcpyAsync(denominator_d,&denominator_h[0],1*sizeof(PRISMATIC_FLOAT_PRECISION),cudaMemcpyHostToDevice));
		
		//reduce in X
		DPC_numerator_reduce <<< (dimj * dimi - 1) / BLOCK_SIZE1D + 1, BLOCK_SIZE1D, 0, stream >>>
		(psiIntensity_ds,qxa_d, num_qx_d, dimj * dimi);
		
		//reduce in Y
		DPC_numerator_reduce <<< (dimj * dimi - 1) / BLOCK_SIZE1D + 1, BLOCK_SIZE1D, 0, stream >>>
		(psiIntensity_ds,qya_d, num_qy_d, dimj * dimi);
		
		DPC_denominator_reduce <<< (dimj * dimi - 1) / BLOCK_SIZE1D + 1, BLOCK_SIZE1D, 0, stream >>> (psiIntensity_ds, denominator_d, dimj*dimi);
		
		//copy back to host
		cudaErrchk(cudaMemcpyAsync(&num_qx_h[0],num_qx_d,1*sizeof(PRISMATIC_FLOAT_PRECISION),cudaMemcpyDeviceToHost));
		cudaErrchk(cudaMemcpyAsync(&num_qy_h[0],num_qy_d,1*sizeof(PRISMATIC_FLOAT_PRECISION),cudaMemcpyDeviceToHost));
		cudaErrchk(cudaMemcpyAsync(&denominator_h[0],denominator_d,1*sizeof(PRISMATIC_FLOAT_PRECISION),cudaMemcpyDeviceToHost));

		PRISMATIC_FLOAT_PRECISION DPC_CoM[2];
		DPC_CoM[0] = num_qx_h[0]/denominator_h[0]; //measurement at ax,ay of CoM w.r.t. qx
		DPC_CoM[1] = num_qy_h[0]/denominator_h[0]; //measurement at ax,ay of CoM w.r.t. qy

		//copy to memory and free variables
		const size_t dpc_stack_offset = 
				currentSlice*pars.DPC_CoM.get_dimk() * pars.DPC_CoM.get_dimj() * pars.DPC_CoM.get_dimi() + ay * pars.DPC_CoM.get_dimj() * pars.DPC_CoM.get_dimi() + ax * pars.DPC_CoM.get_dimi();
		memcpy(&pars.DPC_CoM[dpc_stack_offset],&DPC_CoM[0],2*sizeof(PRISMATIC_FLOAT_PRECISION));
		cudaErrchk(cudaFree(num_qx_d));
		cudaErrchk(cudaFree(num_qy_d));
		cudaErrchk(cudaFree(denominator_d));
		free(num_qx_h);
		free(num_qy_h);
		free(denominator_h);
		//cudaErrchk(cudaMemcpyAsync());
	}
}

size_t getNextPower2(const size_t& val){
	size_t p = 0;
	while (pow(2,p) <= val)++p;
	return p;
}