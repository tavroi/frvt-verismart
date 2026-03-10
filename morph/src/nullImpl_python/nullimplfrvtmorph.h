/*
 * This software was developed at the National Institute of Standards and
 * Technology (NIST) by employees of the Federal Government in the course
 * of their official duties. Pursuant to title 17 Section 105 of the
 * United States Code, this software is not subject to copyright protection
 * and is in the public domain. NIST assumes no responsibility whatsoever for
 * its use by other parties, and makes no guarantees, expressed or implied,
 * about its quality, reliability, or any other characteristic.
 */

#ifndef NULLIMPLFRVTMORPH_H_
#define NULLIMPLFRVTMORPH_H_

#include <pybind11/embed.h>
#include <pybind11/numpy.h>
#include <iostream>
#include <mutex>
#include "frvt_morph.h" 

namespace FRVT_MORPH {
	class NullImplFRVTMorph : public FRVT_MORPH::Interface {
public:
    NullImplFRVTMorph();
	~NullImplFRVTMorph() override;

    FRVT::ReturnStatus
	initialize(
		const std::string &configDir, 
		const std::string &configValue) override;

    FRVT::ReturnStatus
		detectMorph(
		const FRVT::Image &suspectedMorph,
		const FRVT_MORPH::ImageLabel &label,
		bool &isMorph,
		double &score) override;

	FRVT::ReturnStatus
	detectMorphDifferentially(
		const FRVT::Image &suspectedMorph,
		const FRVT_MORPH::ImageLabel &label,
		const FRVT::Image &liveFace,
		bool &isMorph,
		double &score) override;

	FRVT::ReturnStatus
	detectMorphDifferentially(
		const FRVT::Image &suspectedMorph,
		const FRVT_MORPH::ImageLabel &label,
		const FRVT::Image &liveFace,
		const FRVT_MORPH::SubjectMetadata &subjectMetadata,
		bool &isMorph,
		double &score) override;

	FRVT::ReturnStatus
	compareImages(
		const FRVT::Image &enrollImage,
		const FRVT::Image &verifImage,
		double &similarity) override;

    FRVT::ReturnStatus
    demorph(
        const FRVT::Image &suspectedMorph,
        FRVT::Image &outputSubject1,
        FRVT::Image &outputSubject2,
        bool &isMorph,
        double &score) override;

    FRVT::ReturnStatus
    demorphDifferentially(
        const FRVT::Image &suspectedMorph,
        const FRVT::Image &probeFace,
        FRVT::Image &outputSubject,
        bool &isMorph,
        double &score) override;

	static std::shared_ptr<FRVT_MORPH::Interface> 
	getImplementation();
private:
    bool initialized;
    std::mutex init_mutex;
    pybind11::module_ python_model;
};
}

#endif /* NULLIMPLFRVTMORPH_H_ */
