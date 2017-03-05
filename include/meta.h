//
// Created by AJ Pryor on 3/2/17.
//

#ifndef PRISM_META_H
#define PRISM_META_H
#include <vector>
#include <string>
#include <cstddef>
namespace PRISM{
	template <class T>
	class Metadata{
	public:
		Metadata(){
			interpolationFactor = 5;
			filename_atoms      = "";
			filename_output     = "";
			realspace_pixelSize = 100.0 / 1000.0;
			pixelSize = std::vector<T>{realspace_pixelSize,realspace_pixelSize}; // remove this parameter maybe
			potBound = 1.0;
			numFP = 8.0 / 8.0;
			sliceThickness = 2;
			cellDim = std::vector<size_t>{0,0,0}; // this is z,y,x format
			E0 = 80e3;
			alphaBeamMax = 24 / 1000.0;
			NUM_GPUS = 1;
			NUM_THREADS = 12;
		}
		size_t interpolationFactor;
		std::string filename_atoms;
		std::string filename_output;
		T realspace_pixelSize;
		std::vector<T> pixelSize;
		T potBound;
		size_t numFP;
		T sliceThickness;
		std::vector<size_t> cellDim; // this is z,y,x format
		T E0;
		T alphaBeamMax;
		size_t NUM_GPUS;
		size_t NUM_THREADS;
	};
}
#endif //PRISM_META_H
