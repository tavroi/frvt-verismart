/*
 * This software was developed at the National Institute of Standards and
 * Technology (NIST) by employees of the Federal Government in the course
 * of their official duties. Pursuant to title 17 Section 105 of the
 * United States Code, this software is not subject to copyright protection
 * and is in the public domain. NIST assumes no responsibility  whatsoever for
 * its use by other parties, and makes no guarantees, expressed or implied,
 * about its quality, reliability, or any other characteristic.
 */

#ifndef NULLIMPLFRVT11_TWINS_H_
#define NULLIMPLFRVT11_TWINS_H_

#include "frvt11.h"

/*
 * Declare the implementation class of the FRVT Twins Interface
 */
namespace FRVT_11 {
    class NullImplFRVT11_TWINS : public FRVT_11::Interface {
public:

    NullImplFRVT11_TWINS();
    ~NullImplFRVT11_TWINS() override;

    FRVT::ReturnStatus
    initialize(const std::string &configDir) override;

    FRVT::ReturnStatus
    createFaceTemplate(
            const std::vector<FRVT::Image> &faces,
            FRVT::TemplateRole role,
            std::vector<uint8_t> &templ,
            std::vector<FRVT::EyePair> &eyeCoordinates) override;

    FRVT::ReturnStatus
    createIrisTemplate(
            const std::vector<FRVT::Image> &irises,
            FRVT::TemplateRole role,
            std::vector<uint8_t> &templ,
            std::vector<FRVT::IrisAnnulus> &irisLocations) override;

    FRVT::ReturnStatus
    createFaceTemplate(
            const FRVT::Image &image,
            FRVT::TemplateRole role,
            std::vector<std::vector<uint8_t>> &templs,
            std::vector<FRVT::EyePair> &eyeCoordinates) override;

    FRVT::ReturnStatus
    matchTemplates(
            const std::vector<uint8_t> &verifTemplate,
            const std::vector<uint8_t> &enrollTemplate,
            double &score) override;

    static std::shared_ptr<FRVT_11::Interface>
    getImplementation();

private:
    std::string configDir;
    static const int featureVectorSize{4};
    // Some other members
};
}

#endif /* NULLIMPLFRVT11_TWINS_H_ */
