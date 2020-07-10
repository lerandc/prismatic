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

#include "projectedPotential.h"
#include <vector>
#include "ArrayND.h"
#include "boost/math/special_functions/bessel.hpp"
#include <math.h>
#include <iostream>
#include <algorithm>
#include <numeric>
#include "kirkland_params.h"
#include "defines.h"
#include "configure.h"

namespace Prismatic
{

PRISMATIC_FLOAT_PRECISION get_potMin(const Array2D<PRISMATIC_FLOAT_PRECISION> &pot,
									 const Array1D<PRISMATIC_FLOAT_PRECISION> &xr,
									 const Array1D<PRISMATIC_FLOAT_PRECISION> &yr)
{
	//xr and yr are generated symmetric about 0
	//TODO: decide to bifurcate into legacy functionality?

	const size_t xInd = std::floor(xr.size() / 2);
	const size_t yInd = std::floor(yr.size() / 2);
	// const PRISMATIC_FLOAT_PRECISION dx = round(sqrt(2 * (xInd + 1) - 1));
	// const PRISMATIC_FLOAT_PRECISION dy = round(sqrt(2 * (yInd + 1) - 1));
	// const PRISMATIC_FLOAT_PRECISION xv[] = {xInd - dx, xInd + dx, xInd - dx, xInd + dx, 0, 0, (PRISMATIC_FLOAT_PRECISION)xr.size() - 1, (PRISMATIC_FLOAT_PRECISION)xr.size() - 1};
	// const PRISMATIC_FLOAT_PRECISION yv[] = {0, 0, (PRISMATIC_FLOAT_PRECISION)yr.size() - 1, (PRISMATIC_FLOAT_PRECISION)yr.size() - 1, yInd - dy, yInd + dy, yInd - dy, yInd + dy};

	const PRISMATIC_FLOAT_PRECISION xv[] = {(PRISMATIC_FLOAT_PRECISION)xr.size() - 2, xInd}; //-2 to gaurantee zero faces
	const PRISMATIC_FLOAT_PRECISION yv[] = {yInd, (PRISMATIC_FLOAT_PRECISION)yr.size() - 2};

	PRISMATIC_FLOAT_PRECISION potMin = 0;
	for (auto i = 0; i < 2; ++i)
		potMin = (pot.at(yv[i], xv[i]) > potMin) ? pot.at(yv[i], xv[i]) : potMin;
	return potMin;
}

PRISMATIC_FLOAT_PRECISION get_potMin3D(const Array3D<PRISMATIC_FLOAT_PRECISION>& pot,
                                     const Array1D<PRISMATIC_FLOAT_PRECISION>& xr,
                                     const Array1D<PRISMATIC_FLOAT_PRECISION>& yr,
                                     const Array1D<PRISMATIC_FLOAT_PRECISION>& zr)
{	
	//xr, yr, and zr are generated symmetric about zero
	//looks for minimum potential to prevent interaction with vacuum edge at boundaries of potential integration
	const size_t xInd = std::floor(xr.size() / 2);
	const size_t yInd = std::floor(yr.size() / 2);
	const size_t zInd = std::floor(zr.size() / 2);

	const PRISMATIC_FLOAT_PRECISION xv[] = {(PRISMATIC_FLOAT_PRECISION)xr.size() - 2, xInd, xInd}; // -2 to guarantee zero on face
	const PRISMATIC_FLOAT_PRECISION yv[] = {yInd, (PRISMATIC_FLOAT_PRECISION)yr.size() - 2, yInd};
	const PRISMATIC_FLOAT_PRECISION zv[] = {zInd, zInd, (PRISMATIC_FLOAT_PRECISION)zr.size() - 2};

	PRISMATIC_FLOAT_PRECISION potMin = 0;
	for (auto i = 0; i < 3; ++i)
		potMin = (pot.at(zv[i], yv[i], xv[i]) > potMin) ? pot.at(zv[i], yv[i], xv[i]) : potMin;
	return potMin;
};

using namespace std;

Array2D<PRISMATIC_FLOAT_PRECISION> projPot(const size_t &Z,
										   const Array1D<PRISMATIC_FLOAT_PRECISION> &xr,
										   const Array1D<PRISMATIC_FLOAT_PRECISION> &yr)
{
	// compute the projected potential for a given atomic number following Kirkland

	// setup some constants
	static const PRISMATIC_FLOAT_PRECISION pi = std::acos(-1);
	PRISMATIC_FLOAT_PRECISION ss = 8;
	PRISMATIC_FLOAT_PRECISION a0 = 0.5292;
	PRISMATIC_FLOAT_PRECISION e = 14.4;
	PRISMATIC_FLOAT_PRECISION term1 = 4 * pi * pi * a0 * e;
	PRISMATIC_FLOAT_PRECISION term2 = 2 * pi * pi * a0 * e;

	// initialize array
	ArrayND<2, std::vector<PRISMATIC_FLOAT_PRECISION>> result = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{yr.size(), xr.size()}});

	// setup some coordinates
	const PRISMATIC_FLOAT_PRECISION dx = xr[1] - xr[0];
	const PRISMATIC_FLOAT_PRECISION dy = yr[1] - yr[0];

	PRISMATIC_FLOAT_PRECISION start = -(ss - 1) / ss / 2;
	const PRISMATIC_FLOAT_PRECISION step = 1 / ss;
	const PRISMATIC_FLOAT_PRECISION end = -start;
	vector<PRISMATIC_FLOAT_PRECISION> sub_data;
	while (start <= end)
	{
		sub_data.push_back(start);
		start += step;
	}
	ArrayND<1, std::vector<PRISMATIC_FLOAT_PRECISION>> sub(sub_data, {{sub_data.size()}});

	std::pair<Array2D<PRISMATIC_FLOAT_PRECISION>, Array2D<PRISMATIC_FLOAT_PRECISION>> meshx = meshgrid(xr, sub * dx);
	std::pair<Array2D<PRISMATIC_FLOAT_PRECISION>, Array2D<PRISMATIC_FLOAT_PRECISION>> meshy = meshgrid(yr, sub * dy);

	ArrayND<1, std::vector<PRISMATIC_FLOAT_PRECISION>> xv = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{meshx.first.size()}});
	ArrayND<1, std::vector<PRISMATIC_FLOAT_PRECISION>> yv = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{meshy.first.size()}});
	{
		auto t_x = xv.begin();
		for (auto j = 0; j < meshx.first.get_dimj(); ++j)
		{
			for (auto i = 0; i < meshx.first.get_dimi(); ++i)
			{
				*t_x++ = meshx.first.at(j, i) + meshx.second.at(j, i);
			}
		}
	}

	{
		auto t_y = yv.begin();
		for (auto j = 0; j < meshy.first.get_dimj(); ++j)
		{
			for (auto i = 0; i < meshy.first.get_dimi(); ++i)
			{
				*t_y++ = meshy.first.at(j, i) + meshy.second.at(j, i);
			}
		}
	}

	std::pair<Array2D<PRISMATIC_FLOAT_PRECISION>, Array2D<PRISMATIC_FLOAT_PRECISION>> meshxy = meshgrid(yv, xv);
	ArrayND<2, std::vector<PRISMATIC_FLOAT_PRECISION>> r2 = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{yv.size(), xv.size()}});
	ArrayND<2, std::vector<PRISMATIC_FLOAT_PRECISION>> r = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{yv.size(), xv.size()}});

	{
		auto t_y = r2.begin();
		for (auto j = 0; j < meshxy.first.get_dimj(); ++j)
		{
			for (auto i = 0; i < meshxy.first.get_dimi(); ++i)
			{
				*t_y++ = pow(meshxy.first.at(j, i), 2) + pow(meshxy.second.at(j, i), 2);
			}
		}
	}

	for (auto i = 0; i < r.size(); ++i) r[i] = sqrt(r2[i]);
	// construct potential
	ArrayND<2, std::vector<PRISMATIC_FLOAT_PRECISION>> potSS = ones_ND<2, PRISMATIC_FLOAT_PRECISION>({{r2.get_dimj(), r2.get_dimi()}});

	// get the relevant table values
	std::vector<PRISMATIC_FLOAT_PRECISION> ap;
	ap.resize(NUM_PARAMETERS);
	for (auto i = 0; i < NUM_PARAMETERS; ++i)
	{
		ap[i] = fparams[(Z - 1) * NUM_PARAMETERS + i];
	}

	// compute the potential
	using namespace boost::math;
	std::transform(r.begin(), r.end(),
				   r2.begin(), potSS.begin(), [&ap, &term1, &term2](const PRISMATIC_FLOAT_PRECISION &r_t, const PRISMATIC_FLOAT_PRECISION &r2_t) {
					   return term1 * (ap[0] *
										   cyl_bessel_k(0, 2 * pi * sqrt(ap[1]) * r_t) +
									   ap[2] * cyl_bessel_k(0, 2 * pi * sqrt(ap[3]) * r_t) +
									   ap[4] * cyl_bessel_k(0, 2 * pi * sqrt(ap[5]) * r_t)) +
							  term2 * (ap[6] / ap[7] * exp(-pow(pi, 2) / ap[7] * r2_t) +
									   ap[8] / ap[9] * exp(-pow(pi, 2) / ap[9] * r2_t) +
									   ap[10] / ap[11] * exp(-pow(pi, 2) / ap[11] * r2_t));
				   });

	// integrate
	ArrayND<2, std::vector<PRISMATIC_FLOAT_PRECISION>> pot = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{yr.size(), xr.size()}});
	for (auto sy = 0; sy < ss; ++sy)
	{
		for (auto sx = 0; sx < ss; ++sx)
		{
			for (auto j = 0; j < pot.get_dimj(); ++j)
			{
				for (auto i = 0; i < pot.get_dimi(); ++i)
				{
					pot.at(j, i) += potSS.at(j * ss + sy, i * ss + sx);
				}
			}
		}
	}
	pot /= (ss * ss);

	PRISMATIC_FLOAT_PRECISION potMin = get_potMin(pot, xr, yr);
	pot -= potMin;
	transform(pot.begin(), pot.end(), pot.begin(), [](PRISMATIC_FLOAT_PRECISION &a) { return a < 0 ? 0 : a; });

	return pot;
}

Array3D<PRISMATIC_FLOAT_PRECISION> kirklandPotential3D(const size_t &Z, 
										const Array1D<PRISMATIC_FLOAT_PRECISION> &xr,
										const Array1D<PRISMATIC_FLOAT_PRECISION> &yr,
										const Array1D<PRISMATIC_FLOAT_PRECISION> &zr)
{
	static const PRISMATIC_FLOAT_PRECISION pi = std::acos(-1);
	PRISMATIC_FLOAT_PRECISION a0 = 0.529; //bohr radius
	PRISMATIC_FLOAT_PRECISION e = 14.4; //electron charge in Volt-Angstoms
	PRISMATIC_FLOAT_PRECISION term1 =  2*pi*pi*a0*e;
	PRISMATIC_FLOAT_PRECISION term2 = 2*pow(pi,5.0/2.0)*a0*e;
	
	// get the relevant table values
	std::vector<PRISMATIC_FLOAT_PRECISION> ap;
	ap.resize(NUM_PARAMETERS);
	for (auto i = 0; i < NUM_PARAMETERS; ++i)
	{
		ap[i] = fparams[(Z - 1) * NUM_PARAMETERS + i];
	}

	//construct arrays with supersampling
	PRISMATIC_FLOAT_PRECISION ss = 8;
	const PRISMATIC_FLOAT_PRECISION dx = xr[1] - xr[0];
	const PRISMATIC_FLOAT_PRECISION dy = yr[1] - yr[0];
	const PRISMATIC_FLOAT_PRECISION dz = zr[1] - zr[0];

	PRISMATIC_FLOAT_PRECISION start = -(ss - 1) / ss / 2;
	const PRISMATIC_FLOAT_PRECISION step = 1 / ss;
	const PRISMATIC_FLOAT_PRECISION end = -start;
	vector<PRISMATIC_FLOAT_PRECISION> sub_data;
	while (start <= end)
	{
		sub_data.push_back(start);
		start += step;
	}
	ArrayND<1, std::vector<PRISMATIC_FLOAT_PRECISION>> sub(sub_data, {{sub_data.size()}});

	std::pair<Array2D<PRISMATIC_FLOAT_PRECISION>, Array2D<PRISMATIC_FLOAT_PRECISION>> meshx = meshgrid(xr, sub * dx);
	std::pair<Array2D<PRISMATIC_FLOAT_PRECISION>, Array2D<PRISMATIC_FLOAT_PRECISION>> meshy = meshgrid(yr, sub * dy);
	std::pair<Array2D<PRISMATIC_FLOAT_PRECISION>, Array2D<PRISMATIC_FLOAT_PRECISION>> meshz = meshgrid(zr, sub * dz);

	ArrayND<1, std::vector<PRISMATIC_FLOAT_PRECISION>> xv = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{meshx.first.size()}});
	ArrayND<1, std::vector<PRISMATIC_FLOAT_PRECISION>> yv = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{meshy.first.size()}});
	ArrayND<1, std::vector<PRISMATIC_FLOAT_PRECISION>> zv = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{meshz.first.size()}});
	{
		auto t_x = xv.begin();
		for (auto j = 0; j < meshx.first.get_dimj(); ++j)
		{
			for (auto i = 0; i < meshx.first.get_dimi(); ++i)
			{
				*t_x++ = meshx.first.at(j, i) + meshx.second.at(j, i);
			}
		}
	}

	{
		auto t_y = yv.begin();
		for (auto j = 0; j < meshy.first.get_dimj(); ++j)
		{
			for (auto i = 0; i < meshy.first.get_dimi(); ++i)
			{
				*t_y++ = meshy.first.at(j, i) + meshy.second.at(j, i);
			}
		}
	}

	{
		auto t_z = zv.begin();
		for (auto j = 0; j < meshz.first.get_dimj(); ++j)
		{
			for (auto i = 0; i < meshz.first.get_dimi(); ++i)
			{
				*t_z++ = meshz.first.at(j, i) + meshz.second.at(j, i);
			}
		}
	}

	std::tuple<Array3D<PRISMATIC_FLOAT_PRECISION>, Array3D<PRISMATIC_FLOAT_PRECISION>, Array3D<PRISMATIC_FLOAT_PRECISION>> meshxyz = meshgrid(zv, yv, xv);
	Array3D<PRISMATIC_FLOAT_PRECISION> r2 = zeros_ND<3, PRISMATIC_FLOAT_PRECISION>({{zv.size(), yv.size(), xv.size()}});
	Array3D<PRISMATIC_FLOAT_PRECISION> r  = zeros_ND<3, PRISMATIC_FLOAT_PRECISION>({{zv.size(), yv.size(), xv.size()}});

	//calculate radius
	{
		auto t_y = r2.begin();
		for (auto k = 0; k < std::get<0>(meshxyz).get_dimk(); k++)
		{
			for (auto j = 0; j < std::get<0>(meshxyz).get_dimj(); j++)
			{
				for (auto i = 0; i < std::get<0>(meshxyz).get_dimi(); i++)
				{
					*t_y++ = pow(std::get<0>(meshxyz).at(k, j, i), 2)
							+ pow(std::get<1>(meshxyz).at(k, j, i), 2)
							+ pow(std::get<2>(meshxyz).at(k, j, i), 2);
				}
			}
		}
	}

	for (auto i = 0; i < r.size(); ++i) r[i] = sqrt(r2[i]);
	
	Array3D<PRISMATIC_FLOAT_PRECISION> potSS = zeros_ND<3,PRISMATIC_FLOAT_PRECISION>({{r2.get_dimk(), r2.get_dimj(), r2.get_dimi()}});

	std::transform(r2.begin(),r2.end(),r.begin(),potSS.begin(),[&ap,&term1,&term2](const PRISMATIC_FLOAT_PRECISION &r2, const PRISMATIC_FLOAT_PRECISION &r){
		return term1*(ap[0]*exp(-2*pi*r*sqrt(ap[1]))/r
						+ ap[2]*exp(-2*pi*r*sqrt(ap[3]))/r
						+ ap[4]*exp(-2*pi*r*sqrt(ap[5]))/r)
			+ term2*(ap[6]*pow(ap[7],-3.0/2.0)*exp(-pi*pi*r2/ap[7])
					+ ap[8]*pow(ap[9],-3.0/2.0)*exp(-pi*pi*r2/ap[9])
					+ ap[10]*pow(ap[11],-3.0/2.0)*exp(-pi*pi*r2/ap[11]));
	});

	//integrate
	Array3D<PRISMATIC_FLOAT_PRECISION> pot = zeros_ND<3,PRISMATIC_FLOAT_PRECISION>({{zr.size(), yr.size(), xr.size()}});
	for (auto sz = 0; sz < ss; ++sz)
	{
		for (auto sy = 0; sy < ss; ++sy)
		{
			for (auto sx = 0; sx < ss; ++sx)
			{
				for (auto k = 0; k < pot.get_dimk(); ++k)
				{
					for (auto j = 0; j < pot.get_dimj(); ++j)
					{
						for (auto i = 0; i < pot.get_dimi(); ++i)
						{
							pot.at(k, j, i) += potSS.at(k * ss + sz, j * ss + sy, i * ss + sx);
						}
					}
				}
			}
		}
	}

	pot /= ss*ss*ss;
	PRISMATIC_FLOAT_PRECISION potMin = get_potMin3D(pot, xr, yr, zr);
	pot -= potMin;
	//keep potential if it is positive, else, zero
	std::transform(pot.begin(), pot.end(), pot.begin(), [](PRISMATIC_FLOAT_PRECISION &pot){ return pot < 0 ? 0 : pot;});
	return pot;
}

} // namespace Prismatic
