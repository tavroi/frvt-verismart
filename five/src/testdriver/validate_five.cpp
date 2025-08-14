/**
 * This software was developed at the National Institute of Standards and
 * Technology (NIST) by employees of the Federal Government in the course
 * of their official duties. Pursuant to title 17 Section 105 of the
 * United States Code, this software is not subject to copyright protection
 * and is in the public domain. NIST assumes no responsibility whatsoever for
 * its use by other parties, and makes no guarantees, expressed or implied,
 * about its quality, reliability, or any other characteristic.
 */

#include <fstream>
#include <iostream>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <sstream>
#include <iomanip>
#include <unordered_set>
#include <limits>

#include "frte_five.h"
#include "util.h"

using namespace std;
using namespace FIVE;

const int candListLength{20};
const std::string candListHeader{"searchId candidateRank searchRetCode isAssigned templateId score"};

std::map<std::string, FIVE::Image::ImageDescription> mapFiveStringToImgLabel =
{
    { "unknown", FIVE::Image::ImageDescription::Unknown },
    { "stilliso", FIVE::Image::ImageDescription::StillISO },
    { "stillmugshot", FIVE::Image::ImageDescription::StillMugshot },
    { "stillphotojournalism", FIVE::Image::ImageDescription::StillPhotojournalism },
    { "stillwild", FIVE::Image::ImageDescription::StillWild },
    { "videolongrange", FIVE::Image::ImageDescription::VideoLongRange },
    { "videophotojournalism", FIVE::Image::ImageDescription::VideoPhotojournalism },
    { "videopassiveobservation", FIVE::Image::ImageDescription::VideoPassiveObservation },
    { "videochokepoint", FIVE::Image::ImageDescription::VideoChokepoint },
    { "videoelevatedplatform", FIVE::Image::ImageDescription::VideoElevatedPlatform },
};

std::map<std::string, FIVE::Media::Label> mapFiveStringToMediaLabel =
{
    { "image", FIVE::Media::Label::Image },
    { "video", FIVE::Media::Label::Video },
};

bool
readFiveImage(
    const std::string &file,
    FIVE::Image &image)
{
    /* Open PPM file. */
    ifstream input(file, ios::binary);
    if (!input.is_open()) {
        std::cerr << "[ERROR] Cannot open image: " << file << std::endl;
        return false;
    }

    /* Read in magic number. */
    std::string magicNumber;
    input >> magicNumber;
    if (magicNumber != "P6" && magicNumber != "P5") {
        std::cerr << "[ERROR] Error reading magic number from file." << std::endl;
        return false;
    }

    uint16_t maxValue;
    /* Read in image width, height, and max intensity value. */
    input >> image.width >> image.height >> maxValue;
    if (!input.good()) {
        std::cerr << "[ERROR] Error, premature end of file while reading header." << std::endl;
        return false;
    }

    if (magicNumber == "P5")
        image.depth = 8;
    else if (magicNumber == "P6")
        image.depth = 24;

    /* Skip line break. */
    input.ignore(numeric_limits<streamsize>::max(), '\n');

    uint8_t *data = new uint8_t[image.size()];
    image.data.reset(data, std::default_delete<uint8_t[]>());

    /* Read in raw pixel data. */
    input.read((char*)image.data.get(), image.size());
    if (!input.good()) {
        std::cerr << "[ERROR] Error, only read " << input.gcount() << " bytes." << std::endl;
        return false;
    }
    return true;
}

int
enroll(shared_ptr<Interface> &implPtr,
    const std::string &configDir,
    const std::string &inputFile,
    const std::string &outputLog,
    const std::string &edb,
    const std::string &manifest)
{
    /* Read input file */
    ifstream inputStream(inputFile);
    if (!inputStream.is_open()) {
        std::cerr << "[ERROR] Failed to open stream for " << inputFile << "." << std::endl;
	raise(SIGTERM);
    }

    /* Open output log for writing */
    ofstream logStream(outputLog);
    if (!logStream.is_open()) {
        std::cerr << "[ERROR] Failed to open stream for " << outputLog << "." << std::endl;
        raise(SIGTERM);
    }

    /* header */
    logStream << "id image templateSizeBytes returnCode "
	    "bbxleft bbytop bbwidth bbheight" << std::endl;

    /* Open EDB file for writing */
    ofstream edbStream(edb);
    if (!edbStream.is_open()) {
        std::cerr << "[ERROR] Failed to open stream for " << edb << "." << std::endl;
        raise(SIGTERM);
    }

    /* Open manifest for writing */
    ofstream manifestStream(manifest);
    if (!manifestStream.is_open()) {
        std::cerr << "[ERROR] Failed to open stream for " << manifest << "." << std::endl;
        raise(SIGTERM);
    }

    std::string id, line;
    FIVE::ReturnStatus ret;

    while (std::getline(inputStream, line)) {
        auto tokens = split(line, '|');
        id = tokens[0];

        std::vector<FIVE::Media> mediaVector;
        std::vector< std::vector<std::string> > imageNames;
        for (unsigned int i = 1; i < tokens.size(); i++) {
            std::vector<std::string> names;
            auto mediaEntry = split(tokens[i], ' ');
            FIVE::Media media;
            /* Either image or video */
            media.type = mapFiveStringToMediaLabel[mediaEntry[0]];
            if (media.type == FIVE::Media::Label::Image)
                media.fps = 0;
            else if (media.type == FIVE::Media::Label::Video)
                media.fps = 30;

            /* Get number of stills/frames in mediaEntry */
            auto numImages = (mediaEntry.size() - 1)/2;
            for (unsigned int j = 0; j < numImages; j++) {
                FIVE::Image image;
                std::string imagePath = mediaEntry[(j*2)+1];
                names.push_back(imagePath);
                std::string desc = mediaEntry[(j*2)+2];
                if (!readFiveImage(imagePath, image)) {
                    std::cerr << "[ERROR] Failed to load image file: " << imagePath << "." << std::endl;
                    raise(SIGTERM);
                }
                image.description = mapFiveStringToImgLabel[desc];
                media.data.push_back(image);
            }
            imageNames.push_back(names);
            mediaVector.push_back(media);
        }
        std::vector<uint8_t> templ;
        std::vector< std::vector<FIVE::BoundingBox> > boundingBoxes;

        ret = implPtr->createEnrollmentTemplate(mediaVector, templ, boundingBoxes);
        /* If function is not implemented, raise error */
        if (ret.code == ReturnCode::NotImplemented) {
            std::cerr << "[ERROR] createEnrollmentTemplate() must be implemented!" << std::endl;
            raise(SIGTERM);
        }

        /* Write to edb and manifest */
        manifestStream << id << " "
                << templ.size() << " "
                << edbStream.tellp() << std::endl;
        edbStream.write(
                (char*)templ.data(),
                templ.size());

        /* If function returns non-successful return code or no bounding boxes 
         * are returned, fill the std::vector with default values */
        if (ret.code != ReturnCode::Success || (mediaVector.size() != boundingBoxes.size())) {
            boundingBoxes.clear();
            for (unsigned int i = 0; i < mediaVector.size(); i++) {
                auto allBBs = std::vector<FIVE::BoundingBox>(mediaVector[i].data.size());
                boundingBoxes.push_back(allBBs);
            }
        }

        for (unsigned int i = 0; i < mediaVector.size(); i++) {
            auto media = mediaVector[i].data;
            auto bbs = boundingBoxes[i]; 
            for (unsigned int j = 0; j < media.size(); j++) {
                /* Write template stats to log */
                std::string imagePath = imageNames[i][j];
                logStream << id << " "
                        << imagePath << " "
                        << templ.size() << " "
                        << static_cast<std::underlying_type<ReturnCode>::type>(ret.code) << " ";
                logStream << bbs[j].xleft << " "
                        << bbs[j].ytop << " "
                        << bbs[j].width << " "
                        << bbs[j].height << " ";
                logStream << std::endl;
            }
        }
    }
    inputStream.close();

    /* Remove the input file */
    if( remove(inputFile.c_str()) != 0 )
        std::cerr << "[ERROR] Error deleting file: " << inputFile << std::endl;

    return SUCCESS;
}

int
finalize(shared_ptr<Interface> &implPtr,
    const std::string &edbDir,
    const std::string &enrollDir,
    const std::string &configDir)
{
    std::string edb{edbDir+"/edb"}, manifest{edbDir+"/manifest"};
    /* Check file existence of edb and manifest */
    if (!(ifstream(edb) && ifstream(manifest))) {
        std::cerr << "[ERROR] EDB file: " << edb << " and/or manifest file: "
                << manifest << " is missing." << std::endl;
        raise(SIGTERM);
    }

    auto ret = implPtr->finalizeEnrollment(configDir, enrollDir, edb, manifest, GalleryType::Unconsolidated);
    if (ret.code != ReturnCode::Success) {
        std::cerr << "[ERROR] finalizeEnrollment() returned error code: "
                << ret.code << "." << std::endl;
        raise(SIGTERM);
    }
    return SUCCESS;
}

void
printCandidateList(
    const std::string &key,
    const std::vector<FIVE::Candidate> &candList)
{
    std::string printList;
    int i = 0;
    for (auto &c : candList) {
        std::ostringstream streamObj;
        streamObj << std::fixed << std::setprecision(10);
        streamObj << c.score;
        std::string scoreStr = streamObj.str();
        printList += key + " " + std::to_string(i++) + " " + c.templateId + " " + scoreStr + "\n";
    }
    std::cerr << printList << std::endl;
}

void 
checkCandidateList(
    const std::string &key,
    const std::vector<FIVE::Candidate> &candList,
    const int &candListLength)
{   
    if (candList.size() != (unsigned int)candListLength) {
        std::cerr << "[ERROR] The number of returned candidates: " << candList.size()
            << " is not the same as the number of requested candidates: "
            << candListLength << std::endl; 
        raise(SIGTERM);
    }

    /* Check for duplicate candidate IDs */
    double thisScore, lastScore = candList[0].score;
    std::unordered_set<std::string> dupCheck;
    for (auto &c : candList) {
        dupCheck.insert(c.templateId);

        /* While we're iterating, ensure scores in order */
        if (c.isAssigned) {
            thisScore = c.score;
            if (lastScore < thisScore) {
                std::cerr << "[ERROR] Similarity scores are not sorted in descending order." << std::endl;
                printCandidateList(key, candList);
                raise(SIGTERM);
            }
            lastScore = thisScore;
        } 
    }

    if (dupCheck.size() != candList.size()) {
        std::cerr << "[ERROR] Duplicate template IDs exist in the candidate list (this is not allowed!)." << std::endl;
        printCandidateList(key, candList);
        raise(SIGTERM);
    }
}

void
searchAndLog(
    shared_ptr<Interface> &implPtr,
    const string &id,
    const vector<uint8_t> &templ,
    ofstream &candListStream,
    const FIVE::ReturnStatus &templGenRet)
{
    std::vector<FIVE::Candidate> candidateList;
    FIVE::ReturnStatus ret;

    /* If a valid search template was generated */
    if (templGenRet.code == ReturnCode::Success) {
        ret = implPtr->search(
                templ,
                candListLength,
                candidateList);
        if (ret.code != ReturnCode::Success) {
            /* Populate candidate list with null entries */
            candidateList.resize(candListLength, Candidate(false, "NA", -1.0));
        }
    } else {
        ret = templGenRet;
        /* Populate candidate list with null entries */
        candidateList.resize(candListLength, Candidate(false, "NA", -1.0));
    }

    if (ret.code == ReturnCode::Success)
        checkCandidateList(id, candidateList, candListLength);

    /* Write to candidate list file */
    int i{0};
    for (const auto& candidate : candidateList)
        candListStream << id << " " << i++ << " "
        << static_cast<underlying_type<ReturnCode>::type>(ret.code) << " "
        << candidate.isAssigned << " "
        << candidate.templateId << " "
        << candidate.score << endl;
}

int
search(shared_ptr<Interface> &implPtr,
    const std::string &configDir,
    const std::string &enrollDir,
    const std::string &inputFile,
    const std::string &candList,
    const Action &action)
{
    /* Read probes */
    ifstream inputStream(inputFile);
    if (!inputStream.is_open()) {
        std::cerr << "[ERROR] Failed to open stream for " << inputFile << "." << std::endl;
       	raise(SIGTERM); 
    }

    /* Open candidate list log for writing */
    ofstream candListStream(candList);
    if (!candListStream.is_open()) {
        std::cerr << "[ERROR] Failed to open stream for " << candList << "." << std::endl;
        raise(SIGTERM);
    }
    /* header */
    candListStream << candListHeader << std::endl;

    /* Process each probe */
    std::string id, imagePath, desc, line;
    FIVE::ReturnStatus ret;

    while (std::getline(inputStream, line)) {
        auto tokens = split(line, '|');
        id = tokens[0];

        std::vector<std::string> imageNames;
        if (tokens.size() > 2) {
            std::cerr << "[ERROR] Detected more than one media entry for probe!" << std::endl;
            raise(SIGTERM);
        }

        std::vector<std::string> names;
        auto mediaEntry = split(tokens[1], ' ');
        FIVE::Media media;
        /* Either image or video */
        media.type = mapFiveStringToMediaLabel[mediaEntry[0]];
        if (media.type == FIVE::Media::Label::Image)
            media.fps = 0;
        else if (media.type == FIVE::Media::Label::Video)
            media.fps = 30;

        /* Get number of stills/frames in mediaEntry */
        auto numImages = (mediaEntry.size() - 1)/2;
        for (unsigned int j = 0; j < numImages; j++) {
            FIVE::Image image;
            std::string imagePath = mediaEntry[(j*2)+1];
            names.push_back(imagePath);
            std::string desc = mediaEntry[(j*2)+2];
            if (!readFiveImage(imagePath, image)) {
                std::cerr << "[ERROR] Failed to load image file: " << imagePath << "." << std::endl;
                raise(SIGTERM);
            }
            image.description = mapFiveStringToImgLabel[desc];
            media.data.push_back(image);
        }

        std::vector< std::vector<uint8_t> > templs;
        std::vector< std::vector<FIVE::BoundingBox> > boundingBoxes;

        ret = implPtr->createSearchTemplate(media, templs, boundingBoxes);
            
        if (ret.code == ReturnCode::NotImplemented) {
            std::cerr << "[ERROR] createSearchTemplate() must be implemented!" << std::endl;
            raise(SIGTERM); 
        }

        if (ret.code != ReturnCode::Success) {
            templs.clear();
            templs.push_back(std::vector<uint8_t>());
        }

        /* For each template generated, do search and log results to candidatelist file */
        for (unsigned int i = 0; i < templs.size(); i++) {
            string templID = id + "_" + to_string(i);
            searchAndLog(implPtr, templID, templs[i], candListStream, ret);
        }
    }
    inputStream.close();

    /* Remove the input file */
    if( remove(inputFile.c_str()) != 0 )
        std::cerr << "[ERROR] Error deleting file: " << inputFile << std::endl;

    return SUCCESS;
}

void usage(const std::string &executable)
{
    std::cerr << "Usage: " << executable << " enroll_1N|finalize_1N|search_1N -c configDir -e enrollDir "
            "-o outputDir -h outputStem -i inputFile -t numForks" << std::endl;
    exit(EXIT_FAILURE);
}

int
initialize(
    shared_ptr<Interface> &implPtr,
    const std::string &configDir,
    const std::string &enrollDir,
    Action action)
{
    if (action == Action::Enroll_1N || action == Action::Search_1N) {
        /* Initialization */
        auto ret = implPtr->initializeTemplateCreation(configDir);
        if (ret.code != ReturnCode::Success) {
            std::cerr << "[ERROR] initializeTemplateCreation() returned error code: "
                    << ret.code << "." << std::endl;
            raise(SIGTERM);
        }
        if (action == Action::Search_1N) {
            /* Initialize search */
            ret = implPtr->initializeSearch(configDir, enrollDir);
            if (ret.code != ReturnCode::Success) {
                std::cerr << "[ERROR] initializeSearch() returned error code: "
                        << ret.code << "." << std::endl;
                raise(SIGTERM);
            }
        }
    } 

    return SUCCESS;
}

int
main(int argc, char* argv[])
{
    auto exitStatus = SUCCESS;

    uint16_t currAPIMajorVersion{1},
	currAPIMinorVersion{0},
	currStructsMajorVersion{1},
	currStructsMinorVersion{0};

    /* Check versioning of both five_structs.h and API header file */
    if ((FIVE::FIVE_STRUCTS_MAJOR_VERSION != currStructsMajorVersion) ||
	(FIVE::FIVE_STRUCTS_MINOR_VERSION != currStructsMinorVersion)) {
	std::cerr << "[ERROR] You've compiled your library with an old version of the five_structs.h file: version " <<
	    FIVE::FIVE_STRUCTS_MAJOR_VERSION << "." <<
	    FIVE::FIVE_STRUCTS_MINOR_VERSION <<
	    ".  Please re-build with the latest version: " <<
	    currStructsMajorVersion << "." <<
	    currStructsMinorVersion << "." << std::endl;
	return (FAILURE);
    }

    if ((FIVE::API_MAJOR_VERSION != currAPIMajorVersion) ||
	(FIVE::API_MINOR_VERSION != currAPIMinorVersion)) {
	std::cerr << "[ERROR] You've compiled your library with an old version of the API header file: " <<
	    FIVE::API_MAJOR_VERSION << "." <<
	    FIVE::API_MINOR_VERSION <<
	    ".  Please re-build with the latest version:" <<
	    currAPIMajorVersion << "." <<
	    currStructsMinorVersion << "." << std::endl;
	return (FAILURE);
    }

    int requiredArgs = 2; /* exec name, action */
    if (argc < requiredArgs)
	usage(argv[0]);

    std::string
        actionstr{argv[1]},
    	configDir{"config"},
    	enrollDir{"enroll"},
    	outputDir{"output"},
    	outputFileStem{"stem"},
    	inputFile;
    int numForks = 1;

    for (int i = 0; i < argc - requiredArgs; i++) {
        if (strcmp(argv[requiredArgs+i],"-c") == 0)
            configDir = argv[requiredArgs+(++i)];
        else if (strcmp(argv[requiredArgs+i],"-e") == 0)
            enrollDir = argv[requiredArgs+(++i)];
        else if (strcmp(argv[requiredArgs+i],"-o") == 0)
            outputDir = argv[requiredArgs+(++i)];
        else if (strcmp(argv[requiredArgs+i],"-h") == 0)
            outputFileStem = argv[requiredArgs+(++i)];
        else if (strcmp(argv[requiredArgs+i],"-i") == 0)
            inputFile = argv[requiredArgs+(++i)];
        else if (strcmp(argv[requiredArgs+i],"-t") == 0)
            numForks = atoi(argv[requiredArgs+(++i)]);
        else {
            std::cerr << "Unrecognized flag: " << argv[requiredArgs+i] << std::endl;;
            usage(argv[0]);
        }
    }

    Action action = mapStringToAction[actionstr];
    switch (action) {
        case Action::Enroll_1N:
        case Action::Finalize_1N:
        case Action::Search_1N:
            break;
        default:
            std::cerr << "[ERROR] Unknown command: " << actionstr << std::endl;
            usage(argv[0]);
    }

    auto implPtr = Interface::getImplementation();
    if (action == Action::Enroll_1N || action == Action::Search_1N) {
        /* Initialization */
        if (initialize(implPtr, configDir, enrollDir, action) != EXIT_SUCCESS)
            return EXIT_FAILURE;

        /* Split input file into appropriate number of splits */
        std::vector<std::string> inputFileVector;
        if (splitInputFile(inputFile, outputDir, numForks, inputFileVector) != EXIT_SUCCESS) {
            std::cerr << "[ERROR] An error occurred with processing the input file." << std::endl;
            return EXIT_FAILURE;
        }

        bool parent = false;
        int i = 0;
        for (auto &inputFile : inputFileVector) {
            /* Fork */
            switch(fork()) {
            case 0: /* Child */
                if (action == Action::Enroll_1N)
                    return enroll(
                            implPtr,
                            configDir,
                            inputFile,
                            outputDir + "/" + outputFileStem + "." + mapActionToString[action] + "." + std::to_string(i),
                            outputDir + "/edb." + std::to_string(i),
                            outputDir + "/manifest." + std::to_string(i));
                else if (action == Action::Search_1N) 
                    return search(
                            implPtr,
                            configDir,
                            enrollDir,
                            inputFile,
                            outputDir + "/" + outputFileStem + "." + mapActionToString[action] + "." + std::to_string(i),
                            action);
            case -1: /* Error */
                std::cerr << "[ERROR] Problem forking" << std::endl;
                break;
            default: /* Parent */
                parent = true;
                break;
            }
            i++;
        }

        /* Parent -- wait for children */
        if (parent) {
            while (numForks > 0) {
                int stat_val;
                pid_t cpid;

                cpid = wait(&stat_val);
                if (WIFEXITED(stat_val)) { exitStatus = WEXITSTATUS(stat_val); }
                else if (WIFSIGNALED(stat_val)) {
                    std::cerr << "PID " << cpid << " exited due to signal " <<
                    WTERMSIG(stat_val) << std::endl;
                    exitStatus = FAILURE;
                } else {
                    std::cerr << "PID " << cpid << " exited with unknown status." << std::endl;
                    exitStatus = FAILURE;
                }

                numForks--;
            }
        }
    } else if (action == Action::Finalize_1N) {
        return finalize(implPtr, outputDir, enrollDir, configDir);
    } 

    return exitStatus;
}
