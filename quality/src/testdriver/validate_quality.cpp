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
#include <iterator>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>


#include "frvt_quality.h"
#include "util.h"

using namespace std;
using namespace FRVT;
using namespace FRVT_QUALITY;

int
runQuality(
    std::shared_ptr<Interface> &implPtr,
    const string &inputFile,
    const string &outputLog,
    Action action)
{
    /* Read input file */
    ifstream inputStream(inputFile);
    if (!inputStream.is_open()) {
        cerr << "[ERROR] Failed to open stream for " << inputFile << "." << endl;
        raise(SIGTERM);
    }

    /* Open output log for writing */
    ofstream logStream(outputLog);
    if (!logStream.is_open()) {
        cerr << "[ERROR] Failed to open stream for " << outputLog << "." << endl;
        raise(SIGTERM);
    }

    /* header */
    if (action == Action::VectorQ) {
        logStream << "id image returnCode bb_xleft bb_ytop bb_width bb_height ";
        for (QualityMeasure e = QualityMeasure::Begin; e != QualityMeasure::End; ++e) {
            logStream << e << " "; 
        }
        logStream << endl;
    }       

    string id, imagePath, desc;
    ReturnStatus ret;
    while (inputStream >> id >> imagePath >> desc) {
        Image image;
        if (!readImage(imagePath, image)) {
            cerr << "[ERROR] Failed to load image file: " << imagePath << "." << endl;
            raise(SIGTERM);
        }
        image.description = mapStringToImgLabel[desc];

        Image face{image};
        ImageQualityAssessment assessments;
        if (action == Action::VectorQ)
            ret = implPtr->vectorQuality(face, assessments);
        
        /* If function is not implemented, clean up and exit */
        if (ret.code == ReturnCode::NotImplemented) {
            break;
        }

        if (action == Action::VectorQ) {
            logStream << id << " "
                << imagePath << " "
                << static_cast<std::underlying_type<ReturnCode>::type>(ret.code) << " ";

            auto detection = assessments.qAssessments;
            auto bb = assessments.boundingBox; 
            logStream << (to_string(bb.xleft)) << " " << (to_string(bb.ytop)) << " " << (to_string(bb.width)) << " " << (to_string(bb.height));
            for (QualityMeasure e = QualityMeasure::Begin; e != QualityMeasure::End; ++e) {
                auto it = detection.find(e);
                logStream << " " << ((it != detection.end()) ? to_string(it->second) : "NA");
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

void usage(const string &executable)
{
    cerr << "Usage: " << executable << " -c configDir "
            "-o outputDir -h outputStem -i inputFile -t numForks" << endl;
    exit(EXIT_FAILURE);
}

int
main(
        int argc,
        char* argv[])
{
    auto exitStatus = SUCCESS;

    uint16_t currAPIMajorVersion{4},
        currAPIMinorVersion{1},
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

    if ((FRVT_QUALITY::API_MAJOR_VERSION != currAPIMajorVersion) ||
            (FRVT_QUALITY::API_MINOR_VERSION != currAPIMinorVersion)) {
        cerr << "[ERROR] You've compiled your library with an old version of the API header file: " <<
            FRVT_QUALITY::API_MAJOR_VERSION << "." <<
            FRVT_QUALITY::API_MINOR_VERSION <<
            ".  Please re-build with the latest version:" <<
            currAPIMajorVersion << "." <<
            currStructsMinorVersion << "." << endl;
        return (FAILURE);
    }

    int requiredArgs = 2; /* exec name and action */
    if (argc < requiredArgs)
        usage(argv[0]);

    string actionstr{argv[1]},
        configDir{"config"},
        outputDir{"output"},
        outputFileStem{"stem"},
        inputFile;
    int numForks = 1;

    for (int i = 0; i < argc - requiredArgs; i++) {
        if (strcmp(argv[requiredArgs+i],"-c") == 0)
            configDir = argv[requiredArgs+(++i)];
        else if (strcmp(argv[requiredArgs+i],"-o") == 0)
            outputDir = argv[requiredArgs+(++i)];
        else if (strcmp(argv[requiredArgs+i],"-h") == 0)
            outputFileStem = argv[requiredArgs+(++i)];
        else if (strcmp(argv[requiredArgs+i],"-i") == 0)
            inputFile = argv[requiredArgs+(++i)];
        else if (strcmp(argv[requiredArgs+i],"-t") == 0)
            numForks = atoi(argv[requiredArgs+(++i)]);
        else {
            cerr << "[ERROR] Unrecognized flag: " << argv[requiredArgs+i] << endl;;
            usage(argv[0]);
        }
    }

    Action action = mapStringToAction[actionstr];
    switch(action) {
        case Action::VectorQ:
            break;
        default:
            cerr << "Unknown command: " << actionstr << endl;
            usage(argv[0]);
    }

    /* Get implementation pointer */
    auto implPtr = Interface::getImplementation();
    /* Initialization */
    auto ret = implPtr->initialize(configDir);
    if (ret.code != ReturnCode::Success) {
        cerr << "[ERROR] initialize() returned error: "
                << ret.code << "." << endl;
        return FAILURE;
    }

    /* Split input file into appropriate number of splits */
    vector<string> inputFileVector;
    if (splitInputFile(inputFile, outputDir, numForks, inputFileVector) != SUCCESS) {
        cerr << "[ERROR] An error occurred with processing the input file." << endl;
        return FAILURE;
    }

    bool parent = false;
    int i = 0;
    for (auto &inputFile : inputFileVector) {
        /* Fork */
        switch(fork()) {
        case 0: /* Child */
            switch (action) {
                case Action::VectorQ:
                    return runQuality(
                        implPtr,
                        inputFile,
                        outputDir + "/" + outputFileStem + ".log." + to_string(i),
                        action);
                default:
                    return FAILURE;
            }
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

    return exitStatus;
}
