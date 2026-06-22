/*
 * This software was developed at the National Institute of Standards and
 * Technology (NIST) by employees of the Federal Government in the course
 * of their official duties. Pursuant to title 17 Section 105 of the
 * United States Code, this software is not subject to copyright protection
 * and is in the public domain. NIST assumes no responsibility  whatsoever for
 * its use by other parties, and makes no guarantees, expressed or implied,
 * about its quality, reliability, or any other characteristic.
 */

#include "nullimplfrvt11.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace FRVT;
using namespace FRVT_11;

namespace {
constexpr std::array<uint8_t, 8> kTemplateMagic{{'F', 'R', 'V', 'T', '1', '1', 'E', '1'}};
constexpr uint16_t kTemplateVersion{1};
constexpr uint16_t kTemplateStatusOk{0};
constexpr uint16_t kTemplateStatusFailed{1};
constexpr uint32_t kExpectedEmbeddingDim{512};
constexpr size_t kTemplateHeaderSize{8 + 2 + 2 + 4 + 4};
constexpr double kEpsilon{1.0e-12};

constexpr std::array<uint8_t, 4> kRequestMagic{{'F', 'R', 'Q', '1'}};
constexpr std::array<uint8_t, 4> kResponseMagic{{'F', 'R', 'S', '1'}};
constexpr std::array<uint8_t, 8> kWorkerReady{{'F', 'R', 'V', 'T', 'P', 'Y', '1', '\n'}};
constexpr uint32_t kCmdCreateSinglePerson{1};
constexpr uint32_t kCmdCreateMultiPerson{2};
constexpr uint32_t kCmdQuit{99};
constexpr uint32_t kWorkerStatusOk{0};

void appendU16LE(std::vector<uint8_t> &out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xffU));
    out.push_back(static_cast<uint8_t>((v >> 8U) & 0xffU));
}

void appendU32LE(std::vector<uint8_t> &out, uint32_t v)
{
    for (unsigned int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((v >> (i * 8U)) & 0xffU));
}

void appendU64LE(std::vector<uint8_t> &out, uint64_t v)
{
    for (unsigned int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((v >> (i * 8U)) & 0xffU));
}

void appendFloat32LE(std::vector<uint8_t> &out, float v)
{
    static_assert(sizeof(float) == sizeof(uint32_t), "FRVT template format requires IEEE754 float32");
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(float));
    appendU32LE(out, bits);
}

bool readU16LE(const std::vector<uint8_t> &in, size_t &offset, uint16_t &v)
{
    if (offset + 2 > in.size())
        return false;
    v = static_cast<uint16_t>(in[offset]) |
        static_cast<uint16_t>(static_cast<uint16_t>(in[offset + 1]) << 8U);
    offset += 2;
    return true;
}

bool readU32LE(const std::vector<uint8_t> &in, size_t &offset, uint32_t &v)
{
    if (offset + 4 > in.size())
        return false;
    v = static_cast<uint32_t>(in[offset]) |
        (static_cast<uint32_t>(in[offset + 1]) << 8U) |
        (static_cast<uint32_t>(in[offset + 2]) << 16U) |
        (static_cast<uint32_t>(in[offset + 3]) << 24U);
    offset += 4;
    return true;
}

bool readFloat32LE(const std::vector<uint8_t> &in, size_t &offset, float &v)
{
    uint32_t bits = 0;
    if (!readU32LE(in, offset, bits))
        return false;
    std::memcpy(&v, &bits, sizeof(float));
    return true;
}

bool normalizeL2(std::vector<float> &embedding)
{
    double norm2 = 0.0;
    for (const float v : embedding) {
        if (!std::isfinite(v))
            return false;
        norm2 += static_cast<double>(v) * static_cast<double>(v);
    }
    if (norm2 <= kEpsilon)
        return false;

    const float invNorm = static_cast<float>(1.0 / std::sqrt(norm2));
    for (float &v : embedding)
        v *= invNorm;
    return true;
}

std::vector<uint8_t> encodeTemplate(uint16_t status, const std::vector<float> &embedding)
{
    const uint32_t dim = status == kTemplateStatusOk
        ? static_cast<uint32_t>(embedding.size())
        : 0U;
    const uint32_t payloadBytes = dim * static_cast<uint32_t>(sizeof(float));

    std::vector<uint8_t> templ;
    templ.reserve(kTemplateHeaderSize + payloadBytes);
    templ.insert(templ.end(), kTemplateMagic.begin(), kTemplateMagic.end());
    appendU16LE(templ, kTemplateVersion);
    appendU16LE(templ, status);
    appendU32LE(templ, dim);
    appendU32LE(templ, payloadBytes);

    if (status == kTemplateStatusOk) {
        for (const float v : embedding)
            appendFloat32LE(templ, v);
    }
    return templ;
}

std::vector<uint8_t> makeFailureTemplate()
{
    return encodeTemplate(kTemplateStatusFailed, {});
}

bool isFailureTemplate(const std::vector<uint8_t> &templ)
{
    if (templ.size() < kTemplateHeaderSize)
        return true;

    if (!std::equal(kTemplateMagic.begin(), kTemplateMagic.end(), templ.begin()))
        return false;

    size_t offset = kTemplateMagic.size();
    uint16_t version = 0;
    uint16_t status = 0;
    return readU16LE(templ, offset, version) &&
           readU16LE(templ, offset, status) &&
           version == kTemplateVersion &&
           status == kTemplateStatusFailed;
}

bool decodeTemplate(const std::vector<uint8_t> &templ, std::vector<float> &embedding)
{
    embedding.clear();
    if (templ.size() < kTemplateHeaderSize)
        return false;
    if (!std::equal(kTemplateMagic.begin(), kTemplateMagic.end(), templ.begin()))
        return false;

    size_t offset = kTemplateMagic.size();
    uint16_t version = 0;
    uint16_t status = 0;
    uint32_t dim = 0;
    uint32_t payloadBytes = 0;

    if (!readU16LE(templ, offset, version) ||
        !readU16LE(templ, offset, status) ||
        !readU32LE(templ, offset, dim) ||
        !readU32LE(templ, offset, payloadBytes)) {
        return false;
    }

    if (version != kTemplateVersion || status != kTemplateStatusOk)
        return false;
    if (dim != kExpectedEmbeddingDim || payloadBytes != dim * sizeof(float))
        return false;
    if (templ.size() != kTemplateHeaderSize + payloadBytes)
        return false;

    embedding.resize(dim);
    for (float &v : embedding) {
        if (!readFloat32LE(templ, offset, v) || !std::isfinite(v))
            return false;
    }
    return true;
}

std::string resolvePath(const std::string &path)
{
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) != nullptr)
        return std::string(resolved);
    return path;
}

uint16_t toCoord(float v)
{
    if (!std::isfinite(v) || v <= 0.0F)
        return 0;
    const float upper = static_cast<float>(std::numeric_limits<uint16_t>::max());
    return static_cast<uint16_t>(std::lround(std::min(v, upper)));
}

EyePair makeEyePair(bool leftAssigned, bool rightAssigned, float xleft, float yleft, float xright, float yright)
{
    return EyePair(
        leftAssigned,
        rightAssigned,
        toCoord(xleft),
        toCoord(yleft),
        toCoord(xright),
        toCoord(yright));
}

bool writeAll(int fd, const uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(fd, data + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (written == 0)
            return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool readAll(int fd, uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const ssize_t n = read(fd, data + offset, size - offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        offset += static_cast<size_t>(n);
    }
    return true;
}

struct WorkerTemplate {
    bool ok{false};
    std::vector<float> embedding;
};

struct WorkerEye {
    bool leftAssigned{false};
    bool rightAssigned{false};
    float xleft{0.0F};
    float yleft{0.0F};
    float xright{0.0F};
    float yright{0.0F};
};

struct WorkerResponse {
    uint32_t status{kWorkerStatusOk};
    std::string message;
    std::vector<WorkerTemplate> templates;
    std::vector<WorkerEye> eyes;
};

std::vector<uint8_t> makeImageRequest(uint32_t command, const std::vector<Image> &images)
{
    std::vector<uint8_t> request;
    request.reserve(32U);
    request.insert(request.end(), kRequestMagic.begin(), kRequestMagic.end());
    appendU32LE(request, command);
    appendU32LE(request, static_cast<uint32_t>(images.size()));

    for (const Image &image : images) {
        const uint8_t *bytes = image.data.get();
        const size_t sizeBytes = bytes == nullptr ? 0U : image.size();
        appendU32LE(request, image.width);
        appendU32LE(request, image.height);
        appendU32LE(request, image.depth);
        appendU64LE(request, static_cast<uint64_t>(sizeBytes));
        if (sizeBytes > 0)
            request.insert(request.end(), bytes, bytes + sizeBytes);
    }
    return request;
}

} // namespace

namespace FRVT_11 {

class PythonWorker {
public:
    explicit PythonWorker(std::string configDir) :
        configDir_{std::move(configDir)}
    {}

    ~PythonWorker()
    {
        stop();
    }

    bool create(uint32_t command, const std::vector<Image> &images, WorkerResponse &response, std::string &error)
    {
        if (!start(error))
            return false;

        const std::vector<uint8_t> request = makeImageRequest(command, images);
        if (!writeAll(toWorkerFd_, request.data(), request.size())) {
            error = "failed to write request to Python worker";
            stop();
            return false;
        }

        if (!readResponse(response, error)) {
            stop();
            return false;
        }
        return true;
    }

private:
    bool start(std::string &error)
    {
        if (pid_ > 0)
            return true;

        const std::string workerPath = configDir_ + "/frvt11_worker.py";
        std::ifstream workerFile(workerPath);
        if (!workerFile.good()) {
            error = "missing Python worker: " + workerPath;
            return false;
        }

        int toChild[2]{-1, -1};
        int fromChild[2]{-1, -1};
        if (pipe(toChild) != 0 || pipe(fromChild) != 0) {
            error = std::string("pipe() failed: ") + std::strerror(errno);
            if (toChild[0] >= 0) close(toChild[0]);
            if (toChild[1] >= 0) close(toChild[1]);
            if (fromChild[0] >= 0) close(fromChild[0]);
            if (fromChild[1] >= 0) close(fromChild[1]);
            return false;
        }

        pid_ = fork();
        if (pid_ < 0) {
            error = std::string("fork() for Python worker failed: ") + std::strerror(errno);
            close(toChild[0]);
            close(toChild[1]);
            close(fromChild[0]);
            close(fromChild[1]);
            pid_ = -1;
            return false;
        }

        if (pid_ == 0) {
            dup2(toChild[0], STDIN_FILENO);
            dup2(fromChild[1], STDOUT_FILENO);

            close(toChild[0]);
            close(toChild[1]);
            close(fromChild[0]);
            close(fromChild[1]);

            std::string pythonPath = configDir_ + ":" + configDir_ + "/python:" + configDir_ + "/python/site-packages";
            const char *existingPythonPath = getenv("PYTHONPATH");
            if (existingPythonPath != nullptr && existingPythonPath[0] != '\0')
                pythonPath += ":" + std::string(existingPythonPath);

            setenv("PYTHONPATH", pythonPath.c_str(), 1);
            setenv("PYTHONUNBUFFERED", "1", 1);
            setenv("OMP_NUM_THREADS", "1", 0);
            setenv("OPENBLAS_NUM_THREADS", "1", 0);
            setenv("MKL_NUM_THREADS", "1", 0);
            setenv("NUMEXPR_NUM_THREADS", "1", 0);
            setenv("TF_NUM_INTRAOP_THREADS", "1", 0);
            setenv("TF_NUM_INTEROP_THREADS", "1", 0);
            setenv("ORT_INTRA_OP_NUM_THREADS", "1", 0);
            setenv("ORT_INTER_OP_NUM_THREADS", "1", 0);

            execlp("python3", "python3", workerPath.c_str(), "--config", configDir_.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }

        close(toChild[0]);
        close(fromChild[1]);
        toWorkerFd_ = toChild[1];
        fromWorkerFd_ = fromChild[0];

        std::array<uint8_t, kWorkerReady.size()> ready{};
        if (!readAll(fromWorkerFd_, ready.data(), ready.size()) ||
            !std::equal(kWorkerReady.begin(), kWorkerReady.end(), ready.begin())) {
            error = "Python worker did not send ready marker";
            stop();
            return false;
        }

        return true;
    }

    bool readResponse(WorkerResponse &response, std::string &error)
    {
        response = WorkerResponse{};

        std::array<uint8_t, 20> header{};
        if (!readAll(fromWorkerFd_, header.data(), header.size())) {
            error = "failed to read response header from Python worker";
            return false;
        }

        std::vector<uint8_t> headerBytes(header.begin(), header.end());
        if (!std::equal(kResponseMagic.begin(), kResponseMagic.end(), headerBytes.begin())) {
            error = "invalid response magic from Python worker";
            return false;
        }

        size_t offset = kResponseMagic.size();
        uint32_t templateCount = 0;
        uint32_t eyeCount = 0;
        uint32_t messageBytes = 0;
        if (!readU32LE(headerBytes, offset, response.status) ||
            !readU32LE(headerBytes, offset, templateCount) ||
            !readU32LE(headerBytes, offset, eyeCount) ||
            !readU32LE(headerBytes, offset, messageBytes)) {
            error = "malformed response header from Python worker";
            return false;
        }

        if (templateCount > 1024U || eyeCount > 1024U || messageBytes > 65536U) {
            error = "unreasonable response size from Python worker";
            return false;
        }

        if (messageBytes > 0) {
            response.message.resize(messageBytes);
            if (!readAll(fromWorkerFd_, reinterpret_cast<uint8_t *>(&response.message[0]), messageBytes)) {
                error = "failed to read response message from Python worker";
                return false;
            }
        }

        response.templates.reserve(templateCount);
        for (uint32_t i = 0; i < templateCount; ++i) {
            std::array<uint8_t, 8> templHeader{};
            if (!readAll(fromWorkerFd_, templHeader.data(), templHeader.size())) {
                error = "failed to read template record from Python worker";
                return false;
            }

            std::vector<uint8_t> templHeaderBytes(templHeader.begin(), templHeader.end());
            size_t templOffset = 0;
            uint32_t templStatus = 0;
            uint32_t dim = 0;
            if (!readU32LE(templHeaderBytes, templOffset, templStatus) ||
                !readU32LE(templHeaderBytes, templOffset, dim)) {
                error = "malformed template record from Python worker";
                return false;
            }
            if (dim > 4096U) {
                error = "unreasonable embedding dimension from Python worker";
                return false;
            }

            WorkerTemplate templ;
            templ.ok = templStatus == 0U;
            templ.embedding.resize(dim);
            if (dim > 0) {
                std::vector<uint8_t> payload(static_cast<size_t>(dim) * sizeof(float));
                if (!readAll(fromWorkerFd_, payload.data(), payload.size())) {
                    error = "failed to read embedding payload from Python worker";
                    return false;
                }
                size_t payloadOffset = 0;
                for (float &v : templ.embedding) {
                    if (!readFloat32LE(payload, payloadOffset, v)) {
                        error = "malformed embedding payload from Python worker";
                        return false;
                    }
                }
            }
            response.templates.push_back(std::move(templ));
        }

        response.eyes.reserve(eyeCount);
        for (uint32_t i = 0; i < eyeCount; ++i) {
            std::array<uint8_t, 24> eyeRecord{};
            if (!readAll(fromWorkerFd_, eyeRecord.data(), eyeRecord.size())) {
                error = "failed to read eye record from Python worker";
                return false;
            }

            std::vector<uint8_t> eyeBytes(eyeRecord.begin(), eyeRecord.end());
            size_t eyeOffset = 0;
            uint32_t leftAssigned = 0;
            uint32_t rightAssigned = 0;
            WorkerEye eye;
            if (!readU32LE(eyeBytes, eyeOffset, leftAssigned) ||
                !readU32LE(eyeBytes, eyeOffset, rightAssigned) ||
                !readFloat32LE(eyeBytes, eyeOffset, eye.xleft) ||
                !readFloat32LE(eyeBytes, eyeOffset, eye.yleft) ||
                !readFloat32LE(eyeBytes, eyeOffset, eye.xright) ||
                !readFloat32LE(eyeBytes, eyeOffset, eye.yright)) {
                error = "malformed eye record from Python worker";
                return false;
            }
            eye.leftAssigned = leftAssigned != 0U;
            eye.rightAssigned = rightAssigned != 0U;
            response.eyes.push_back(eye);
        }

        return true;
    }

    void stop()
    {
        if (toWorkerFd_ >= 0) {
            std::vector<uint8_t> request;
            request.insert(request.end(), kRequestMagic.begin(), kRequestMagic.end());
            appendU32LE(request, kCmdQuit);
            appendU32LE(request, 0U);
            (void)writeAll(toWorkerFd_, request.data(), request.size());
        }

        if (toWorkerFd_ >= 0) {
            close(toWorkerFd_);
            toWorkerFd_ = -1;
        }
        if (fromWorkerFd_ >= 0) {
            close(fromWorkerFd_);
            fromWorkerFd_ = -1;
        }
        if (pid_ > 0) {
            int status = 0;
            (void)waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

    std::string configDir_;
    pid_t pid_{-1};
    int toWorkerFd_{-1};
    int fromWorkerFd_{-1};
};

} // namespace FRVT_11

namespace {

bool runWorker(
    NullImplFRVT11 &impl,
    std::unique_ptr<PythonWorker> &worker,
    int &workerOwnerPid,
    const std::string &configDir,
    uint32_t command,
    const std::vector<Image> &images,
    WorkerResponse &response,
    std::string &error)
{
    const int currentPid = static_cast<int>(getpid());
    if (workerOwnerPid != currentPid) {
        worker.reset();
        workerOwnerPid = currentPid;
    }

    if (!worker)
        worker = std::make_unique<PythonWorker>(configDir);

    (void)impl;
    if (worker->create(command, images, response, error))
        return true;

    worker.reset();
    worker = std::make_unique<PythonWorker>(configDir);
    return worker->create(command, images, response, error);
}

} // namespace

NullImplFRVT11::NullImplFRVT11() = default;

NullImplFRVT11::~NullImplFRVT11() = default;

ReturnStatus
NullImplFRVT11::initialize(const std::string &configDir)
{
    this->configDir = resolvePath(configDir);
    this->initialized = false;
    this->worker.reset();
    this->workerOwnerPid = 0;

    std::signal(SIGPIPE, SIG_IGN);

    const std::string workerPath = this->configDir + "/frvt11_worker.py";
    std::ifstream workerFile(workerPath);
    if (!workerFile.good())
        return ReturnStatus(ReturnCode::ConfigError, "Missing config/frvt11_worker.py");

    this->initialized = true;
    return ReturnStatus(ReturnCode::Success);
}

ReturnStatus
NullImplFRVT11::createFaceTemplate(
        const std::vector<FRVT::Image> &faces,
        TemplateRole role,
        std::vector<uint8_t> &templ,
        std::vector<EyePair> &eyeCoordinates)
{
    (void)role;
    templ.clear();
    eyeCoordinates.assign(faces.size(), EyePair{});

    if (!initialized) {
        templ = makeFailureTemplate();
        return ReturnStatus(ReturnCode::ConfigError, "initialize() was not called successfully");
    }
    if (faces.empty()) {
        templ = makeFailureTemplate();
        return ReturnStatus(ReturnCode::NumDataError, "No face images supplied");
    }

    WorkerResponse response;
    std::string error;
    if (!runWorker(*this, worker, workerOwnerPid, configDir, kCmdCreateSinglePerson, faces, response, error)) {
        templ = makeFailureTemplate();
        return ReturnStatus(ReturnCode::VendorError, error);
    }

    for (size_t i = 0; i < std::min(eyeCoordinates.size(), response.eyes.size()); ++i) {
        const WorkerEye &eye = response.eyes[i];
        eyeCoordinates[i] = makeEyePair(
            eye.leftAssigned,
            eye.rightAssigned,
            eye.xleft,
            eye.yleft,
            eye.xright,
            eye.yright);
    }

    if (response.status != kWorkerStatusOk) {
        templ = makeFailureTemplate();
        return ReturnStatus(ReturnCode::VendorError, response.message);
    }
    if (response.templates.empty() || !response.templates[0].ok) {
        templ = makeFailureTemplate();
        return ReturnStatus(ReturnCode::Success, "No usable face detected; emitted failure template");
    }

    std::vector<float> embedding = response.templates[0].embedding;
    if (embedding.size() != kExpectedEmbeddingDim || !normalizeL2(embedding)) {
        templ = makeFailureTemplate();
        return ReturnStatus(ReturnCode::VendorError, "Python worker returned invalid embedding");
    }

    templ = encodeTemplate(kTemplateStatusOk, embedding);
    return ReturnStatus(ReturnCode::Success);
}

ReturnStatus
NullImplFRVT11::createIrisTemplate(
        const std::vector<FRVT::Image> &irises,
        TemplateRole role,
        std::vector<uint8_t> &templ,
        std::vector<IrisAnnulus> &irisLocations)
{
    (void)irises;
    (void)role;
    templ = makeFailureTemplate();
    irisLocations.clear();
    return ReturnStatus(ReturnCode::NotImplemented, "This 1:1 submission implements face recognition only");
}

ReturnStatus
NullImplFRVT11::createFaceTemplate(
    const FRVT::Image &image,
    FRVT::TemplateRole role,
    std::vector<std::vector<uint8_t>> &templs,
    std::vector<FRVT::EyePair> &eyeCoordinates)
{
    (void)role;
    templs.clear();
    eyeCoordinates.clear();

    if (!initialized) {
        templs.push_back(makeFailureTemplate());
        eyeCoordinates.push_back(EyePair{});
        return ReturnStatus(ReturnCode::ConfigError, "initialize() was not called successfully");
    }

    WorkerResponse response;
    std::string error;
    if (!runWorker(*this, worker, workerOwnerPid, configDir, kCmdCreateMultiPerson, {image}, response, error)) {
        templs.push_back(makeFailureTemplate());
        eyeCoordinates.push_back(EyePair{});
        return ReturnStatus(ReturnCode::VendorError, error);
    }

    if (response.status != kWorkerStatusOk) {
        templs.push_back(makeFailureTemplate());
        eyeCoordinates.push_back(EyePair{});
        return ReturnStatus(ReturnCode::VendorError, response.message);
    }

    if (response.templates.empty()) {
        templs.push_back(makeFailureTemplate());
        eyeCoordinates.push_back(EyePair{});
        return ReturnStatus(ReturnCode::Success, "No faces detected; emitted failure template");
    }

    for (size_t i = 0; i < response.templates.size(); ++i) {
        const WorkerTemplate &workerTemplate = response.templates[i];
        if (workerTemplate.ok) {
            std::vector<float> embedding = workerTemplate.embedding;
            if (embedding.size() == kExpectedEmbeddingDim && normalizeL2(embedding))
                templs.push_back(encodeTemplate(kTemplateStatusOk, embedding));
            else
                templs.push_back(makeFailureTemplate());
        } else {
            templs.push_back(makeFailureTemplate());
        }

        if (i < response.eyes.size()) {
            const WorkerEye &eye = response.eyes[i];
            eyeCoordinates.push_back(makeEyePair(
                eye.leftAssigned,
                eye.rightAssigned,
                eye.xleft,
                eye.yleft,
                eye.xright,
                eye.yright));
        } else {
            eyeCoordinates.push_back(EyePair{});
        }
    }

    return ReturnStatus(ReturnCode::Success);
}

ReturnStatus
NullImplFRVT11::matchTemplates(
        const std::vector<uint8_t> &verifTemplate,
        const std::vector<uint8_t> &enrollTemplate,
        double &score)
{
    score = -1.0;

    if (!initialized)
        return ReturnStatus(ReturnCode::ConfigError, "initialize() was not called successfully");

    if (isFailureTemplate(verifTemplate) || isFailureTemplate(enrollTemplate))
        return ReturnStatus(ReturnCode::VerifTemplateError, "At least one template encodes failed extraction");

    std::vector<float> verifEmbedding;
    std::vector<float> enrollEmbedding;
    if (!decodeTemplate(verifTemplate, verifEmbedding) || !decodeTemplate(enrollTemplate, enrollEmbedding))
        return ReturnStatus(ReturnCode::TemplateFormatError, "Unable to decode one or both templates");
    if (verifEmbedding.size() != enrollEmbedding.size())
        return ReturnStatus(ReturnCode::TemplateFormatError, "Embedding dimensions differ");

    double dot = 0.0;
    for (size_t i = 0; i < verifEmbedding.size(); ++i)
        dot += static_cast<double>(verifEmbedding[i]) * static_cast<double>(enrollEmbedding[i]);

    if (!std::isfinite(dot))
        return ReturnStatus(ReturnCode::MatchError, "Similarity computation produced a non-finite score");

    score = std::clamp((dot + 1.0) * 0.5, 0.0, 1.0);
    return ReturnStatus(ReturnCode::Success);
}

std::shared_ptr<Interface>
Interface::getImplementation()
{
    return std::make_shared<NullImplFRVT11>();
}
