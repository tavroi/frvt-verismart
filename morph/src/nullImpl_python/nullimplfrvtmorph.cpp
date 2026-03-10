/*
 * This software was developed at the National Institute of Standards and
 * Technology (NIST) by employees of the Federal Government in the course
 * of their official duties. Pursuant to title 17 Section 105 of the
 * United States Code, this software is not subject to copyright protection
 * and is in the public domain. NIST assumes no responsibility whatsoever for
 * its use by other parties, and makes no guarantees, expressed or implied,
 * about its quality, reliability, or any other characteristic.
 */
#include <pybind11/embed.h>
#include <pybind11/numpy.h>
#include <iostream>
#include <mutex>
#include "nullimplfrvtmorph.h"

namespace py = pybind11;
using namespace std;
using namespace FRVT;
using namespace FRVT_MORPH;

NullImplFRVTMorph::NullImplFRVTMorph() {
	initialized = false;
}

NullImplFRVTMorph::~NullImplFRVTMorph() {}

FRVT::ReturnStatus
NullImplFRVTMorph::initialize(
	const std::string &configDir,
	const std::string &configValue) {

	std::lock_guard<std::mutex> lock(init_mutex);

	if (!initialized) {
		try {
			// Start interpreter & add configDir and libDir to sys.path
			py::initialize_interpreter();
			py::module_ sys = py::module_::import("sys");
			// Find the lib directory relative to the config directory
			std::string libDir = configDir + "/../lib";
			sys.attr("path").attr("append")(configDir);
			sys.attr("path").attr("append")(libDir);
		
			//py::exec("import sys\nprint(sys.path)");	
			// Pre-load your model module
			python_model = py::module_::import("morph_detector");
			initialized = true;
		} catch (py::error_already_set &e) {
			std::cerr << "Python Error: " << e.what() << std::endl;
			return ReturnStatus(ReturnCode::ConfigError); 
		}
	}
	return ReturnStatus(ReturnCode::Success);
}

FRVT::ReturnStatus
NullImplFRVTMorph::detectMorph(
    const FRVT::Image &image,
    const FRVT_MORPH::ImageLabel &label,
    bool &isMorph,
    double &score) {
	// NIST calls this from multiple forks! Must acquire GIL.
	py::gil_scoped_acquire acquire;
	try {
		// Convert FRVT::Image (raw bytes) to NumPy (H, W, C)
		py::array_t<uint8_t> numpy_img(
			{ (int)image.height, (int)image.width, (int)image.depth/8 },
			{ (int)(image.width * image.depth/8), (int)image.depth/8, 1 },
			image.data.get(),
			py::cast<py::none>(py::none()) // Tells Python NOT to manage the image.data pointer, because the testdriver manages it
		);

		try {
			py::object result = python_model.attr("do_smad")(numpy_img);
			score = result.cast<double>();
		} catch (py::error_already_set &e) { 
			std::cerr << "Python Error: " << e.what() << std::endl;
            return ReturnStatus(ReturnCode::UnknownError);
		}
		return ReturnStatus(ReturnCode::Success); 
	} catch (const std::exception &e) {
		std::cout << "Exception caught: " << e.what() << std::endl;
		return ReturnStatus(ReturnCode::UnknownError);
	}
}

FRVT::ReturnStatus
NullImplFRVTMorph::detectMorphDifferentially(
	const FRVT::Image &suspectedMorph,
	const FRVT_MORPH::ImageLabel &label,
	const FRVT::Image &liveFace,
	bool &isMorph,
	double &score) {

 	return ReturnStatus(ReturnCode::NotImplemented);
}

FRVT::ReturnStatus
NullImplFRVTMorph::detectMorphDifferentially(
    const FRVT::Image &suspectedMorph,
    const FRVT_MORPH::ImageLabel &label,
    const FRVT::Image &liveFace,
    const FRVT_MORPH::SubjectMetadata &subjectMetadata,
    bool &isMorph,
    double &score) {

	return ReturnStatus(ReturnCode::NotImplemented);
}

FRVT::ReturnStatus
NullImplFRVTMorph::compareImages(
    const Image &enrollImage,
    const Image &verifImage,
    double &similarity) {

	return ReturnStatus(ReturnCode::NotImplemented);
}

FRVT::ReturnStatus
NullImplFRVTMorph::demorph(
	const FRVT::Image &suspectedMorph,
	FRVT::Image &outputSubject1,
	FRVT::Image &outputSubject2,
	bool &isMorph,
	double &score) {
	return ReturnStatus(ReturnCode::NotImplemented);
}

FRVT::ReturnStatus
NullImplFRVTMorph::demorphDifferentially(
	const FRVT::Image &suspectedMorph,
	const FRVT::Image &probeFace,
	FRVT::Image &outputSubject,
	bool &isMorph,
	double &score) {
	return ReturnStatus(ReturnCode::NotImplemented);
}
 
// Factory function required by NIST
std::shared_ptr<Interface>
Interface::getImplementation()
{
    return std::make_shared<NullImplFRVTMorph>();
}
