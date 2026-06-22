/*
 * FRVT 1:1 implementation — pure C++ ONNX Runtime inference.
 * No fork, no subprocess, no GPU, no stdout/stderr.
 */

#ifndef NULLIMPLFRVT11_H_
#define NULLIMPLFRVT11_H_

#include "frvt11.h"
#include <memory>
#include <string>

namespace FRVT_11 {

class FaceEngine;

class NullImplFRVT11 : public FRVT_11::Interface {
public:
    NullImplFRVT11();
    ~NullImplFRVT11() override;

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
    FaceEngine &getEngine();

    std::string configDir_;
    bool initialized_{false};
    int enginePid_{0};
    std::unique_ptr<FaceEngine> engine_;
};

} // namespace FRVT_11

#endif /* NULLIMPLFRVT11_H_ */
