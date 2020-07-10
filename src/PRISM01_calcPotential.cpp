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

#include "PRISM01_calcPotential.h"
#include "params.h"
#include <iostream>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <map>
#include <vector>
#include <random>
#include <thread>
#include "params.h"
#include "ArrayND.h"
#include "projectedPotential.h"
#include "WorkDispatcher.h"
#include "utility.h"

#ifdef PRISMATIC_BUILDING_GUI
#include "prism_progressbar.h"
#endif

namespace Prismatic
{
using namespace std;
void fetch_potentials(Array3D<PRISMATIC_FLOAT_PRECISION> &potentials,
					  const vector<size_t> &atomic_species,
					  const Array1D<PRISMATIC_FLOAT_PRECISION> &xr,
					  const Array1D<PRISMATIC_FLOAT_PRECISION> &yr)
{
	Array2D<PRISMATIC_FLOAT_PRECISION> cur_pot;
	for (auto k = 0; k < potentials.get_dimk(); ++k)
	{
		Array2D<PRISMATIC_FLOAT_PRECISION> cur_pot = projPot(atomic_species[k], xr, yr);
		for (auto j = 0; j < potentials.get_dimj(); ++j)
		{
			for (auto i = 0; i < potentials.get_dimi(); ++i)
			{
				potentials.at(k, j, i) = cur_pot.at(j, i);
			}
		}
	}
}

vector<size_t> get_unique_atomic_species(Parameters<PRISMATIC_FLOAT_PRECISION> &pars)
{
	// helper function to get the unique atomic species
	vector<size_t> unique_atoms = vector<size_t>(pars.atoms.size(), 0);
	for (auto i = 0; i < pars.atoms.size(); ++i)
		unique_atoms[i] = pars.atoms[i].species;
	sort(unique_atoms.begin(), unique_atoms.end());
	vector<size_t>::iterator it = unique(unique_atoms.begin(), unique_atoms.end());
	unique_atoms.resize(distance(unique_atoms.begin(), it));
	return unique_atoms;
}

void generateProjectedPotentials(Parameters<PRISMATIC_FLOAT_PRECISION> &pars,
								 const Array3D<PRISMATIC_FLOAT_PRECISION> &potentialLookup,
								 const vector<size_t> &unique_species,
								 const Array1D<long> &xvec,
								 const Array1D<long> &yvec)
{
	// splits the atomic coordinates into slices and computes the projected potential for each.

	// create arrays for the coordinates
	Array1D<PRISMATIC_FLOAT_PRECISION> x = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{pars.atoms.size()}});
	Array1D<PRISMATIC_FLOAT_PRECISION> y = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{pars.atoms.size()}});
	Array1D<PRISMATIC_FLOAT_PRECISION> z = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{pars.atoms.size()}});
	Array1D<PRISMATIC_FLOAT_PRECISION> ID = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{pars.atoms.size()}});
	Array1D<PRISMATIC_FLOAT_PRECISION> sigma = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{pars.atoms.size()}});
	Array1D<PRISMATIC_FLOAT_PRECISION> occ = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{pars.atoms.size()}});

	// populate arrays from the atoms structure
	for (auto i = 0; i < pars.atoms.size(); ++i)
	{
		x[i] = pars.atoms[i].x * pars.tiledCellDim[2];
		y[i] = pars.atoms[i].y * pars.tiledCellDim[1];
		z[i] = pars.atoms[i].z * pars.tiledCellDim[0];
		ID[i] = pars.atoms[i].species;
		sigma[i] = pars.atoms[i].sigma;
		occ[i] = pars.atoms[i].occ;
	}

	// compute the z-slice index for each atom
	auto max_z = std::max_element(z.begin(), z.end());
	Array1D<PRISMATIC_FLOAT_PRECISION> zPlane(z);
	std::transform(zPlane.begin(), zPlane.end(), zPlane.begin(), [&max_z, &pars](PRISMATIC_FLOAT_PRECISION &t_z) {
		return round((-t_z + *max_z) / pars.meta.sliceThickness + 0.5) - 1; // If the +0.5 was to make the first slice z=1 not 0, can drop the +0.5 and -1
	});
	max_z = std::max_element(zPlane.begin(), zPlane.end());
	pars.numPlanes = *max_z + 1;

	//check if intermediate output was specified, if so, create index of output slices
	if (pars.meta.numSlices == 0)
	{
		pars.numSlices = pars.numPlanes;
	}

#ifdef PRISMATIC_BUILDING_GUI
	pars.progressbar->signalPotentialUpdate(0, pars.numPlanes);
#endif

	// initialize the potential array
	pars.pot = zeros_ND<3, PRISMATIC_FLOAT_PRECISION>({{pars.numPlanes, pars.imageSize[0], pars.imageSize[1]}});

	// create a key-value map to match the atomic Z numbers with their place in the potential lookup table
	map<size_t, size_t> Z_lookup;
	for (auto i = 0; i < unique_species.size(); ++i)
		Z_lookup[unique_species[i]] = i;

	//loop over each plane, perturb the atomic positions, and place the corresponding potential at each location
	// using parallel calculation of each individual slice
	std::vector<std::thread> workers;
	workers.reserve(pars.meta.numThreads);

	WorkDispatcher dispatcher(0, pars.numPlanes);
	for (long t = 0; t < pars.meta.numThreads; ++t)
	{
		cout << "Launching thread #" << t << " to compute projected potential slices\n";
		workers.push_back(thread([&pars, &x, &y, &z, &ID, &Z_lookup, &xvec, &sigma, &occ,
								  &zPlane, &yvec, &potentialLookup, &dispatcher]() {
			// create a random number generator to simulate thermal effects
			// std::cout<<"random seed = " << pars.meta.randomSeed << std::endl;
			// srand(pars.meta.randomSeed);
			// std::default_random_engine de(pars.meta.randomSeed);
			// normal_distribution<PRISMATIC_FLOAT_PRECISION> randn(0,1);
			Array1D<long> xp;
			Array1D<long> yp;

			size_t currentSlice, stop;
			currentSlice = stop = 0;
			while (dispatcher.getWork(currentSlice, stop))
			{ // synchronously get work assignment
				Array2D<PRISMATIC_FLOAT_PRECISION> projectedPotential = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{pars.imageSize[0], pars.imageSize[1]}});
				const long dim0 = (long)pars.imageSize[0];
				const long dim1 = (long)pars.imageSize[1];
				while (currentSlice != stop)
				{

					// create a random number generator to simulate thermal effects
					std::cout << "random seed = " << pars.meta.randomSeed + currentSlice * pars.numPlanes << std::endl;
					srand(pars.meta.randomSeed + currentSlice * pars.numPlanes);
					std::default_random_engine de(pars.meta.randomSeed + currentSlice * pars.numPlanes);
					normal_distribution<PRISMATIC_FLOAT_PRECISION> randn(0, 1);

					for (auto atom_num = 0; atom_num < x.size(); ++atom_num)
					{
						if (zPlane[atom_num] == currentSlice)
						{
							if (pars.meta.includeOccupancy)
							{
								if (static_cast<PRISMATIC_FLOAT_PRECISION>(rand()) / static_cast<PRISMATIC_FLOAT_PRECISION>(RAND_MAX) > occ[atom_num])
								{
									continue;
								}
							}
							//								if ( !pars.meta.includeOccupancy || static_cast<PRISMATIC_FLOAT_PRECISION>(rand())/static_cast<PRISMATIC_FLOAT_PRECISION> (RAND_MAX) <= occ[atom_num]) {
							const size_t cur_Z = Z_lookup[ID[atom_num]];
							PRISMATIC_FLOAT_PRECISION X, Y;
							if (pars.meta.includeThermalEffects)
							{ // apply random perturbations
								X = round((x[atom_num] + randn(de) * sigma[atom_num]) / pars.pixelSize[1]);
								Y = round((y[atom_num] + randn(de) * sigma[atom_num]) / pars.pixelSize[0]);
							}
							else
							{
								X = round((x[atom_num]) / pars.pixelSize[1]); // this line uses no thermal factor
								Y = round((y[atom_num]) / pars.pixelSize[0]); // this line uses no thermal factor
							}
							xp = xvec + (long)X;
							for (auto &i : xp)
								i = (i % dim1 + dim1) % dim1; // make sure to get a positive value

							yp = yvec + (long)Y;
							for (auto &i : yp)
								i = (i % dim0 + dim0) % dim0; // make sure to get a positive value
							for (auto ii = 0; ii < xp.size(); ++ii)
							{
								for (auto jj = 0; jj < yp.size(); ++jj)
								{
									// fill in value with lookup table
									projectedPotential.at(yp[jj], xp[ii]) += potentialLookup.at(cur_Z, jj, ii);
								}
							}
							//								}
						}
					}
					// copy the result to the full array
					copy(projectedPotential.begin(), projectedPotential.end(), &pars.pot.at(currentSlice, 0, 0));
#ifdef PRISMATIC_BUILDING_GUI
					pars.progressbar->signalPotentialUpdate(currentSlice, pars.numPlanes);
#endif //PRISMATIC_BUILDING_GUI
					++currentSlice;
				}
			}
		}));
	}
	cout << "Waiting for threads...\n";
	for (auto &t : workers)
		t.join();
#ifdef PRISMATIC_BUILDING_GUI
	pars.progressbar->setProgress(100);
#endif //PRISMATIC_BUILDING_GUI
};

void PRISM01_calcPotential(Parameters<PRISMATIC_FLOAT_PRECISION> &pars)
{
	//builds projected, sliced potential

	// setup some coordinates
	cout << "Entering PRISM01_calcPotential" << endl;
	PRISMATIC_FLOAT_PRECISION yleng = std::ceil(pars.meta.potBound / pars.pixelSize[0]);
	PRISMATIC_FLOAT_PRECISION xleng = std::ceil(pars.meta.potBound / pars.pixelSize[1]);
	ArrayND<1, vector<long>> xvec(vector<long>(2 * (size_t)xleng + 1, 0), {{2 * (size_t)xleng + 1}});
	ArrayND<1, vector<long>> yvec(vector<long>(2 * (size_t)yleng + 1, 0), {{2 * (size_t)yleng + 1}});
	{
		PRISMATIC_FLOAT_PRECISION tmpx = -xleng;
		PRISMATIC_FLOAT_PRECISION tmpy = -yleng;
		for (auto &i : xvec)
			i = tmpx++;
		for (auto &j : yvec)
			j = tmpy++;
	}
	Array1D<PRISMATIC_FLOAT_PRECISION> xr(vector<PRISMATIC_FLOAT_PRECISION>(2 * (size_t)xleng + 1, 0), {{2 * (size_t)xleng + 1}});
	Array1D<PRISMATIC_FLOAT_PRECISION> yr(vector<PRISMATIC_FLOAT_PRECISION>(2 * (size_t)yleng + 1, 0), {{2 * (size_t)yleng + 1}});
	for (auto i = 0; i < xr.size(); ++i)
		xr[i] = (PRISMATIC_FLOAT_PRECISION)xvec[i] * pars.pixelSize[1];
	for (auto j = 0; j < yr.size(); ++j)
		yr[j] = (PRISMATIC_FLOAT_PRECISION)yvec[j] * pars.pixelSize[0];

	vector<size_t> unique_species = get_unique_atomic_species(pars);

	// initialize the lookup table
	Array3D<PRISMATIC_FLOAT_PRECISION> potentialLookup = zeros_ND<3, PRISMATIC_FLOAT_PRECISION>({{unique_species.size(), 2 * (size_t)yleng + 1, 2 * (size_t)xleng + 1}});

	// precompute the unique potentials
	fetch_potentials(potentialLookup, unique_species, xr, yr);

	// populate the slices with the projected potentials
	generateProjectedPotentials(pars, potentialLookup, unique_species, xvec, yvec);

	if (pars.meta.savePotentialSlices)
	{
		//create new datacube group
		H5::Group realslices = pars.outputFile.openGroup("4DSTEM_simulation/data/realslices");
		std::string groupName = "ppotential";
		H5::Group ppotential;
		if (pars.fpFlag == 0)
		{
			ppotential = realslices.createGroup(groupName);

			H5::DataSpace attr_dataspace(H5S_SCALAR);

			int group_type = 1;
			H5::Attribute emd_group_type = ppotential.createAttribute("emd_group_type", H5::PredType::NATIVE_INT, attr_dataspace);
			emd_group_type.write(H5::PredType::NATIVE_INT, &group_type);

			H5::Attribute metadata_group = ppotential.createAttribute("metadata", H5::PredType::NATIVE_INT, attr_dataspace);
			int mgroup = 0;
			metadata_group.write(H5::PredType::NATIVE_INT, &mgroup);

			//write dimensions
			H5::DataSpace str_name_ds(H5S_SCALAR);
			H5::StrType strdatatype(H5::PredType::C_S1, 256);

			hsize_t x_size[1] = {pars.imageSize[1]};
			hsize_t y_size[1] = {pars.imageSize[0]};
			hsize_t z_size[1] = {pars.numPlanes};

			Array1D<PRISMATIC_FLOAT_PRECISION> x_dim_data = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{pars.imageSize[1]}});
			Array1D<PRISMATIC_FLOAT_PRECISION> y_dim_data = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{pars.imageSize[0]}});
			Array1D<PRISMATIC_FLOAT_PRECISION> z_dim_data = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{pars.numPlanes}});

			for (auto i = 0; i < pars.imageSize[1]; i++)
				x_dim_data[i] = i * pars.pixelSize[1];
			for (auto i = 0; i < pars.imageSize[0]; i++)
				y_dim_data[i] = i * pars.pixelSize[0];
			for (auto i = 0; i < pars.numPlanes; i++)
				z_dim_data[i] = i * pars.meta.sliceThickness;

			H5::DataSpace dim1_mspace(1, x_size);
			H5::DataSpace dim2_mspace(1, y_size);
			H5::DataSpace dim3_mspace(1, z_size);

			H5::DataSet dim1;
			H5::DataSet dim2;
			H5::DataSet dim3;

			if (sizeof(PRISMATIC_FLOAT_PRECISION) == sizeof(float))
			{
				dim1 = ppotential.createDataSet("dim1", H5::PredType::NATIVE_FLOAT, dim1_mspace);
				dim2 = ppotential.createDataSet("dim2", H5::PredType::NATIVE_FLOAT, dim2_mspace);
				dim3 = ppotential.createDataSet("dim3", H5::PredType::NATIVE_FLOAT, dim3_mspace);

				H5::DataSpace dim1_fspace = dim1.getSpace();
				H5::DataSpace dim2_fspace = dim2.getSpace();
				H5::DataSpace dim3_fspace = dim3.getSpace();

				dim1.write(&x_dim_data[0], H5::PredType::NATIVE_FLOAT, dim1_mspace, dim1_fspace);
				dim2.write(&y_dim_data[0], H5::PredType::NATIVE_FLOAT, dim2_mspace, dim2_fspace);
				dim3.write(&z_dim_data[0], H5::PredType::NATIVE_FLOAT, dim3_mspace, dim3_fspace);
			}
			else
			{
				dim1 = ppotential.createDataSet("dim1", H5::PredType::NATIVE_DOUBLE, dim1_mspace);
				dim2 = ppotential.createDataSet("dim2", H5::PredType::NATIVE_DOUBLE, dim2_mspace);
				dim3 = ppotential.createDataSet("dim3", H5::PredType::NATIVE_DOUBLE, dim3_mspace);

				H5::DataSpace dim1_fspace = dim1.getSpace();
				H5::DataSpace dim2_fspace = dim2.getSpace();
				H5::DataSpace dim3_fspace = dim3.getSpace();

				dim1.write(&x_dim_data[0], H5::PredType::NATIVE_DOUBLE, dim1_mspace, dim1_fspace);
				dim2.write(&y_dim_data[0], H5::PredType::NATIVE_DOUBLE, dim2_mspace, dim2_fspace);
				dim3.write(&z_dim_data[0], H5::PredType::NATIVE_DOUBLE, dim3_mspace, dim3_fspace);
			}

			//dimension attributes
			const H5std_string dim1_name_str("R_x");
			const H5std_string dim2_name_str("R_y");
			const H5std_string dim3_name_str("R_z");

			H5::Attribute dim1_name = dim1.createAttribute("name", strdatatype, str_name_ds);
			H5::Attribute dim2_name = dim2.createAttribute("name", strdatatype, str_name_ds);
			H5::Attribute dim3_name = dim3.createAttribute("name", strdatatype, str_name_ds);

			dim1_name.write(strdatatype, dim1_name_str);
			dim2_name.write(strdatatype, dim2_name_str);
			dim3_name.write(strdatatype, dim3_name_str);

			const H5std_string dim1_unit_str("[n_m]");
			const H5std_string dim2_unit_str("[n_m]");
			const H5std_string dim3_unit_str("[n_m]");

			H5::Attribute dim1_unit = dim1.createAttribute("units", strdatatype, str_name_ds);
			H5::Attribute dim2_unit = dim2.createAttribute("units", strdatatype, str_name_ds);
			H5::Attribute dim3_unit = dim3.createAttribute("units", strdatatype, str_name_ds);

			dim1_unit.write(strdatatype, dim1_unit_str);
			dim2_unit.write(strdatatype, dim2_unit_str);
			dim3_unit.write(strdatatype, dim3_unit_str);
		}
		else
		{
			ppotential = realslices.openGroup(groupName);
		}

		//read in potential array and stride; also, divide by number of FP to do averaging
		Array3D<PRISMATIC_FLOAT_PRECISION> writeBuffer = zeros_ND<3, PRISMATIC_FLOAT_PRECISION>({{pars.imageSize[1], pars.imageSize[0], pars.numPlanes}});
		for (auto x = 0; x < pars.imageSize[1]; x++)
		{
			for (auto y = 0; y < pars.imageSize[0]; y++)
			{
				for (auto z = 0; z < pars.numPlanes; z++)
				{
					writeBuffer.at(x, y, z) = pars.pot.at(z, y, x) / pars.meta.numFP;
				}
			}
		}

		H5::DataSet potSliceData; //declare out here to avoid scoping
		std::string slice_name = "realslice";
		if (pars.fpFlag == 0)
		{

			//create dataset
			//imageSize[1] is the x dimension
			hsize_t dataDims[3] = {pars.imageSize[1], pars.imageSize[0], pars.numPlanes};
			H5::DataSpace mspace(3, dataDims);

			//switch between float and double, maybe not the best way to do so
			if (sizeof(PRISMATIC_FLOAT_PRECISION) == sizeof(float))
			{
				potSliceData = ppotential.createDataSet(slice_name, H5::PredType::NATIVE_FLOAT, mspace);
			}
			else
			{
				potSliceData = ppotential.createDataSet(slice_name, H5::PredType::NATIVE_DOUBLE, mspace);
			}
		}
		else
		{
			potSliceData = ppotential.openDataSet(slice_name);

			PRISMATIC_FLOAT_PRECISION *readBuffer = (PRISMATIC_FLOAT_PRECISION *)malloc(pars.imageSize[0] * pars.imageSize[1] * pars.numPlanes * sizeof(PRISMATIC_FLOAT_PRECISION));
			H5::DataSpace rfspace = potSliceData.getSpace();
			hsize_t rmdims[3] = {pars.imageSize[1], pars.imageSize[0], pars.numPlanes};
			H5::DataSpace rmspace(3, rmdims);

			if (sizeof(PRISMATIC_FLOAT_PRECISION) == sizeof(float))
			{
				potSliceData.read(&readBuffer[0], H5::PredType::NATIVE_FLOAT, rmspace, rfspace);
			}
			else
			{
				potSliceData.read(&readBuffer[0], H5::PredType::NATIVE_DOUBLE, rmspace, rfspace);
			}

			for (auto i = 0; i < pars.imageSize[0] * pars.imageSize[1] * pars.numPlanes; i++)
				writeBuffer[i] += readBuffer[i];

			free(readBuffer);
			rfspace.close();
			rmspace.close();
		}

		hsize_t wmdims[3] = {pars.imageSize[1], pars.imageSize[0], pars.numPlanes};
		H5::DataSpace wfspace = potSliceData.getSpace();
		H5::DataSpace wmspace(3, wmdims);

		if (sizeof(PRISMATIC_FLOAT_PRECISION) == sizeof(float))
		{
			potSliceData.write(&writeBuffer[0], H5::PredType::NATIVE_FLOAT, wmspace, wfspace);
		}
		else
		{
			potSliceData.write(&writeBuffer[0], H5::PredType::NATIVE_DOUBLE, wmspace, wfspace);
		}
		potSliceData.close();
	}
}
} // namespace Prismatic
