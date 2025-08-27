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
#include <unordered_set>
#include <sstream>
#include <iomanip>

#include <sys/wait.h>
#include <unistd.h>
#include <csignal>

#include "frvt1N.h"
#include "util.h"

using namespace std;
using namespace FRVT;
using namespace FRVT_1N;

const int candListLength{20};
const std::string candListHeader{"searchId candidateRank searchRetCode isAssigned templateId score"};

int
enroll(shared_ptr<Interface> &implPtr,
    const string &configDir,
    const string &inputFile,
    const string &outputLog,
    const string &edb,
    const string &manifest,
    const Modality &modality)
{
    /* Read input file */
    ifstream inputStream(inputFile);
    if (!inputStream.is_open()) {
        cerr << "Failed to open stream for " << inputFile << "." << endl;
	raise(SIGTERM);
    }

    /* Open output log for writing */
    ofstream logStream(outputLog);
    if (!logStream.is_open()) {
        cerr << "Failed to open stream for " << outputLog << "." << endl;
        raise(SIGTERM);
    }

    /* header */
    if (modality == Modality::Face)
        logStream << "id image templateSizeBytes returnCode isLeftEyeAssigned "
            "isRightEyeAssigned xleft yleft xright yright" << endl;
    else if (modality == Modality::Iris)
        logStream << "id image templateSizeBytes returnCode "
            "limbusCenterX limbusCenterY pupilRadius limbusRadius" << endl; 
    else if (modality == Modality::MM)
        logStream << "id image templateSizeBytes returnCode " << endl;

    /* Open EDB file for writing */
    ofstream edbStream(edb);
    if (!edbStream.is_open()) {
        cerr << "Failed to open stream for " << edb << "." << endl;
        raise(SIGTERM);
    }

    /* Open manifest for writing */
    ofstream manifestStream(manifest);
    if (!manifestStream.is_open()) {
        cerr << "Failed to open stream for " << manifest << "." << endl;
        raise(SIGTERM);
    }

    string id, line;
    FRVT::ReturnStatus ret;

    //while (inputStream >> id >> imagePath >> desc) {
    while (std::getline(inputStream, line)) {
        auto tokens = split(line, ' ');
        id = tokens[0];
        // Get number of image entries in line
        auto numImages = (tokens.size() - 1)/2;

        vector<Image> images; 
        for (unsigned int i=0; i<numImages; i++) {
            Image image;
            string imagePath = tokens[(i*2)+1];
            string desc = tokens[(i*2)+2];
            if (!readImage(imagePath, image)) {
                cerr << "Failed to load image file: " << imagePath << "." << endl;
                raise(SIGTERM);
            }
            image.description = mapStringToImgLabel[desc];
            images.push_back(image);
        }

        vector<uint8_t> templ;
        vector<EyePair> eyes;
        vector<IrisAnnulus> irisLocations;

        if (modality == Modality::Face)
            ret = implPtr->createFaceTemplate(images, TemplateRole::Enrollment_1N, templ, eyes);
        else if (modality == Modality::Iris) {
            if (images.size() == 2) {
                images[0].irisLR = Image::IrisLR::LeftIris;
                images[1].irisLR = Image::IrisLR::RightIris;
            }
            ret = implPtr->createIrisTemplate(images, TemplateRole::Enrollment_1N, templ, irisLocations);
        }
        else if (modality == Modality::MM)
            ret = implPtr->createFaceAndIrisTemplate(images, TemplateRole::Enrollment_1N, templ);
            
        /* If function is not implemented, clean up and exit */
        if (ret.code == ReturnCode::NotImplemented) {
            break;
        }

        /* Write to edb and manifest */
        manifestStream << id << " "
                << templ.size() << " "
                << edbStream.tellp() << endl;
        edbStream.write(
                (char*)templ.data(),
                templ.size());

        if (modality == Modality::Face) {
            if (images.size() != eyes.size()) {
                eyes.clear();
                eyes.resize(images.size(), EyePair());
            }   
        } else if (modality == Modality::Iris) {
            if (images.size() != irisLocations.size()) {
                irisLocations.clear();
                irisLocations.resize(images.size(), IrisAnnulus()); 
            }
        }

        for (unsigned int i=0; i<images.size(); i++) {
            /* Write template stats to log */
            string imagePath = tokens[(i*2)+1];
            logStream << id << " "
                    << imagePath << " "
                    << templ.size() << " "
                    << static_cast<std::underlying_type<ReturnCode>::type>(ret.code) << " ";
            if (modality == Modality::Face) {
                logStream << eyes[i].isLeftAssigned << " "
                    << eyes[i].isRightAssigned << " "
                    << eyes[i].xleft << " "
                    << eyes[i].yleft << " "
                    << eyes[i].xright << " "
                    << eyes[i].yright;
            } else if (modality == Modality::Iris) {
                logStream << irisLocations[i].limbusCenterX << " "
                    << irisLocations[i].limbusCenterY << " "
                    << irisLocations[i].pupilRadius << " "
                    << irisLocations[i].limbusRadius; 
            }
            logStream << endl;
        }
    }
    inputStream.close();

    /* Remove the input file */
    if( remove(inputFile.c_str()) != 0 )
        cerr << "Error deleting file: " << inputFile << endl;

    if (ret.code == ReturnCode::NotImplemented) {
        /* Remove the output file */
        logStream.close();
        if( remove(outputLog.c_str()) != 0 )
            cerr << "Error deleting file: " << outputLog << endl;
        return NOT_IMPLEMENTED;
    }
    return SUCCESS;
}

int
finalize(shared_ptr<Interface> &implPtr,
    const string &edbDir,
    const string &enrollDir,
    const string &configDir)
{
    string edb{edbDir+"/edb"}, manifest{edbDir+"/manifest"};
    /* Check file existence of edb and manifest */
    if (!(ifstream(edb) && ifstream(manifest))) {
        cerr << "EDB file: " << edb << " and/or manifest file: "
                << manifest << " is missing." << endl;
        raise(SIGTERM);
    }

    auto ret = implPtr->finalizeEnrollment(configDir, enrollDir, edb, manifest, GalleryType::Unconsolidated);
    if (ret.code != ReturnCode::Success) {
        cerr << "finalizeEnrollment() returned error code: "
                << ret.code << "." << endl;
        raise(SIGTERM);
    }
    return SUCCESS;
}

void
printCandidateList(
    const std::string &key,
    const std::vector<FRVT_1N::Candidate> &candList)
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
    const std::vector<FRVT_1N::Candidate> &candList,
    const int &candListLength,
    const Modality &modality)
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
            if (modality == Modality::Face || modality == Modality::MM) { 
                if (lastScore < thisScore) {
                    std::cerr << "[ERROR] Scores are not sorted in descending order." << std::endl;
                    printCandidateList(key, candList);
                    raise(SIGTERM);
                }
            } else if (modality == Modality::Iris) {
                if (lastScore > thisScore) {
                    std::cerr << "[ERROR] Scores are not sorted in ascending order." << std::endl;
                    printCandidateList(key, candList);
                    raise(SIGTERM);
                }
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
    const FRVT::ReturnStatus &templGenRet,
    const Modality &modality)
{
    vector<Candidate> candidateList;
    FRVT::ReturnStatus ret;

    /* If a valid search template was generated */
    if (templGenRet.code == ReturnCode::Success) {
        ret = implPtr->identifyTemplate(
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
        checkCandidateList(id, candidateList, candListLength, modality);

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
    const string &configDir,
    const string &enrollDir,
    const string &inputFile,
    const string &candList,
    const Action &action,
    const Modality &modality)
{
    /* Read probes */
    ifstream inputStream(inputFile);
    if (!inputStream.is_open()) {
        cerr << "Failed to open stream for " << inputFile << "." << endl;
       	raise(SIGTERM); 
    }

    /* Open candidate list log for writing */
    ofstream candListStream(candList);
    if (!candListStream.is_open()) {
        cerr << "Failed to open stream for " << candList << "." << endl;
        raise(SIGTERM);
    }
    /* header */
    candListStream << candListHeader << endl;

    /* Process each probe */
    string id, imagePath, desc, line;
    FRVT::ReturnStatus ret;

    while (std::getline(inputStream, line)) {
        auto tokens = split(line, ' ');
        id = tokens[0];
        // Get number of image entries in line
        auto numImages = (tokens.size() - 1)/2;

        vector<Image> images;
        for (unsigned int i=0; i<numImages; i++) {
            Image image;
            string imagePath = tokens[(i*2)+1];
            string desc = tokens[(i*2)+2];
            if (!readImage(imagePath, image)) {
                cerr << "Failed to load image file: " << imagePath << "." << endl;
                raise(SIGTERM);
            }
            image.description = mapStringToImgLabel[desc];
            images.push_back(image);
        }

        vector<EyePair> eyes;
        vector<IrisAnnulus> irisLocations;
        if (action == Action::Search_1N) {
            vector<uint8_t> templ;
            if (modality == Modality::Face)
                ret = implPtr->createFaceTemplate(images, TemplateRole::Search_1N, templ, eyes);
            else if (modality == Modality::Iris)
                ret = implPtr->createIrisTemplate(images, TemplateRole::Search_1N, templ, irisLocations);
            else if (modality == Modality::MM)
                ret = implPtr->createFaceAndIrisTemplate(images, TemplateRole::Search_1N, templ);
                
            /* If function is not implemented, clean up and exit */
            if (ret.code == ReturnCode::NotImplemented) {
                break;
            }

            /* Do search and log results to candidatelist file */
            searchAndLog(implPtr, id, templ, candListStream, ret, modality);
        } else if (action == Action::SearchMulti_1N) {
            if (modality != Modality::Face) {
                cerr << "[ERROR] SearchMulti_1N can only be called for the face modality." << endl;
                raise(SIGTERM);
            }

            std::vector<std::vector<uint8_t>> templs;
            auto ret = implPtr->createFaceTemplate(images[0], TemplateRole::Search_1N, templs, eyes);
            /* If function is not implemented, clean up and exit */
            if (ret.code == ReturnCode::NotImplemented) {
                break;
            }

            /* For each template generated, do search and log results to candidatelist file */
            for (unsigned int i = 0; i < templs.size(); i++) {
                string templID = id + "_" + to_string(i);
                searchAndLog(implPtr, templID, templs[i], candListStream, ret, modality);            
            }
        }
    }
    inputStream.close();

    /* Remove the input file */
    if( remove(inputFile.c_str()) != 0 )
        cerr << "Error deleting file: " << inputFile << endl;

    if (ret.code == ReturnCode::NotImplemented) {
        /* Remove the output file */
        candListStream.close();
        if( remove(candList.c_str()) != 0 )
            cerr << "Error deleting file: " << candList << endl;
        return NOT_IMPLEMENTED;
    }
    return SUCCESS;
}

void usage(const string &executable)
{
    cerr << "Usage: " << executable << " face|iris|mm enroll_1N|finalize_1N|search_1N|searchMulti_1N -c configDir -e enrollDir "
            "-o outputDir -h outputStem -i inputFile -t numForks" << endl;
    exit(EXIT_FAILURE);
}

int
initialize(
    shared_ptr<Interface> &implPtr,
    const string &configDir,
    const string &enrollDir,
    Action action)
{
    if (action == Action::Enroll_1N) {
        /* Initialization */
        auto ret = implPtr->initializeTemplateCreation(configDir, TemplateRole::Enrollment_1N);
        if (ret.code != ReturnCode::Success) {
            cerr << "initializeTemplateCreation(TemplateRole::Enrollment_1N) returned error code: "
                    << ret.code << "." << endl;
            raise(SIGTERM);
        }
    } else if (action == Action::Search_1N || action == Action::SearchMulti_1N) {
        /* Initialize probe feature extraction */
        auto ret = implPtr->initializeTemplateCreation(configDir, TemplateRole::Search_1N);
        if (ret.code != ReturnCode::Success) {
            cerr << "initializeTemplateCreation(TemplateRole::Search_1N) returned error code: "
                    << ret.code << "." << endl;
            raise(SIGTERM);
        }

        /* Initialize search */
        ret = implPtr->initializeIdentification(configDir, enrollDir);
        if (ret.code != ReturnCode::Success) {
            cerr << "initializeIdentification() returned error code: "
                    << ret.code << "." << endl;
            raise(SIGTERM);
        }
    }
    return SUCCESS;
}

int
main(int argc, char* argv[])
{
    auto exitStatus = SUCCESS;

    uint16_t currAPIMajorVersion{3},
	currAPIMinorVersion{0},
	currStructsMajorVersion{3},
	currStructsMinorVersion{1};

    /* Check versioning of both frvt_structs.h and API header file */
    if ((FRVT::FRVT_STRUCTS_MAJOR_VERSION != currStructsMajorVersion) ||
	(FRVT::FRVT_STRUCTS_MINOR_VERSION != currStructsMinorVersion)) {
	cerr << "[ERROR] You've compiled your library with an old version of the frvt_structs.h file: version " <<
	    FRVT::FRVT_STRUCTS_MAJOR_VERSION << "." <<
	    FRVT::FRVT_STRUCTS_MINOR_VERSION <<
	    ".  Please re-build with the latest version: " <<
	    currStructsMajorVersion << "." <<
	    currStructsMinorVersion << "." << endl;
	return (FAILURE);
    }

    if ((FRVT_1N::API_MAJOR_VERSION != currAPIMajorVersion) ||
	(FRVT_1N::API_MINOR_VERSION != currAPIMinorVersion)) {
	std::cerr << "[ERROR] You've compiled your library with an old version of the API header file: " <<
	    FRVT_1N::API_MAJOR_VERSION << "." <<
	    FRVT_1N::API_MINOR_VERSION <<
	    ".  Please re-build with the latest version:" <<
	    currAPIMajorVersion << "." <<
	    currStructsMinorVersion << "." << endl;
	return (FAILURE);
    }

    int requiredArgs = 3; /* exec name, modality, action */
    if (argc < requiredArgs)
	usage(argv[0]);

    string
        modalitystr{argv[1]}, 
        actionstr{argv[2]},
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
            cerr << "Unrecognized flag: " << argv[requiredArgs+i] << endl;;
            usage(argv[0]);
        }
    }

    Modality modality = mapStringToModality[modalitystr];
    switch (modality) {
        case Modality::Face:
        case Modality::Iris:
        case Modality::MM:
            break;
        default:
            cerr << "[ERROR] Unknown modality: " << modalitystr << endl;
            usage(argv[0]);
    }

    Action action = mapStringToAction[actionstr];
    switch (action) {
        case Action::Enroll_1N:
        case Action::Finalize_1N:
        case Action::Search_1N:
        case Action::SearchMulti_1N:
            break;
        default:
            cerr << "[ERROR] Unknown command: " << actionstr << endl;
            usage(argv[0]);
    }

    auto implPtr = Interface::getImplementation();
    if (action == Action::Enroll_1N || action == Action::Search_1N || action == Action::SearchMulti_1N) {
        /* Initialization */
        if (initialize(implPtr, configDir, enrollDir, action) != EXIT_SUCCESS)
            return EXIT_FAILURE;

        /* Split input file into appropriate number of splits */
        vector<string> inputFileVector;
        if (splitInputFile(inputFile, outputDir, numForks, inputFileVector) != EXIT_SUCCESS) {
            cerr << "An error occurred with processing the input file." << endl;
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
                            outputDir + "/" + outputFileStem + "." + mapActionToString[action] + "." + to_string(i),
                            outputDir + "/edb." + to_string(i),
                            outputDir + "/manifest." + to_string(i),
                            modality);
                else if (action == Action::Search_1N || action == Action::SearchMulti_1N)
                    return search(
                            implPtr,
                            configDir,
                            enrollDir,
                            inputFile,
                            outputDir + "/" + outputFileStem + "." + mapActionToString[action] + "." + to_string(i),
                            action,
                            modality);
            case -1: /* Error */
                cerr << "Problem forking" << endl;
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
                    cerr << "PID " << cpid << " exited due to signal " <<
                    WTERMSIG(stat_val) << endl;
                    exitStatus = FAILURE;
                } else {
                    cerr << "PID " << cpid << " exited with unknown status." << endl;
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
