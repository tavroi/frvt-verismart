#!/bin/bash

source ../common/scripts/utils.sh

# Function to merge edb and manifest files
# together
function mergeEDB() {
	dir=$1
	edb=$dir/tmp.edb
	manifest=$dir/tmp.manifest
	currOffset=0
	for f in $dir/manifest.*
	do
		seq=`basename $f | awk -F"." '{ print $2 }'`
		while read line
		do
			id=`echo $line | awk '{ print $1 }'`
			size=`echo $line | awk '{ print $2 }'`
			echo "$id $size $currOffset" >> $manifest
			currOffset=$((currOffset+size))
		done < $f	
		cat $dir/edb.$seq >> $edb
	done
	mv $edb $dir/edb
	mv $manifest $dir/manifest

	# Clean up old files	
	rm -rf $dir/edb.* $dir/manifest.*
}

# Make sure there aren't any zombie processes
# left over from previous validation run
kill -9 $(ps -aef | grep "count_thread" | awk '{ print $2 }') 2> /dev/null

configDir=config
if [ ! -e "$configDir" ]; then
	echo "${bold}[ERROR] Missing ./$configDir folder!${normal}"
	exit $failure
fi
# Configuration directory is READ-ONLY
chmod -R 550 $configDir

validationDir=validation
chmod -R 777 $validationDir; rm -rf $validationDir

for modality in face iris mm
do
    # If running iris algorithm, check for existence of iris validation data
    if [ "$modality" == "iris" ] || [ "$modality" == "mm" ]; then
        if [ ! -d "../common/images/iris/images" ]; then
            echo "[ERROR] Missing iris validation data!  Please decrypt and extract NIST_validation_images.tar.gz.gpg in ../common/images/iris and try again.  To obtain the password to decrypt, email irex@nist.gov."
            exit $failure 
        fi
    fi

    outputDir=$validationDir/$modality
    outputStem=validation
    enrollDir=$outputDir/enroll
    mkdir -p $enrollDir

    # Usage: ../bin/validate1N face|iris|mm enroll_1N|finalize_1N|search_1N -c configDir -e enrollDir -o outputDir -h outputStem -i inputFile -t numForks
    #   face|iris|mm: modality (face, iris, or mm (multimodal))
    #   enroll_1N|finalize_1N|search_1N: task to process
    #   configDir: configuration directory
    #   enrollDir: enrollment directory
    #   outputDir: directory where output logs are written to
    #   outputStem: the string to prefix the output filename(s) with
    #   inputFile: input file containing images to process (required for enroll and search tasks)
    #   numForks: number of processes to fork.
    echo "------------------------------"
    echo " Running 1:N validation for $modality"
    echo "------------------------------"

    # Set number of child processes to fork()
    numForks=1

    echo -n "Running Enrollment (Single Process) "
    # Start checking for threading
    ../common/scripts/count_threads.sh validate1N $outputDir/thread.log & pid=$!

    # Enrollment
    inputFile=input/$modality/enroll_1N.txt
    bin/validate1N $modality enroll_1N -c $configDir -o $outputDir -h $outputStem -i $inputFile -t $numForks
    retEnrollment=$?

    # End checking for threading
    kill -9 "$pid"
    wait "$pid" 2>/dev/null

    if [[ $retEnrollment == 0 ]]; then
        echo "[SUCCESS]"
        # Merge output files together
        merge $outputDir/$outputStem enroll_1N
        # Merge edb and manifest together
        mergeEDB $outputDir
    elif [[ $retEnrollment == 2 ]]; then
        echo "[NOT IMPLEMENTED]"
        sleep 2
        chmod -R 777 $outputDir; rm -rf $outputDir;
        continue
    else
        echo "${bold}[ERROR] Enrollment validation (single process) failed${normal}"
        sleep 2
        chmod -R 777 $outputDir; rm -rf $outputDir
        exit $failure
    fi

    maxThreads=$(cat $outputDir/thread.log | sort -u -n | tail -n1)
    # 1 process for testdriver, 1 process for child
    if [ "$maxThreads" -gt "2" ]; then
        echo "${bold}[WARNING] We've detected that your software may be threading or using other multiprocessing techniques during template creation.  The number of threads detected was $maxThreads and it should be 2.  Per the API document, implementations must run single-threaded.  In the test environment, there is no advantage to threading, because NIST will distribute workload across multiple blades and multiple processes.  We highly recommend that you fix this issue prior to submission.${normal}"
    fi
    rm -rf $outputDir; mkdir -p $enrollDir

    echo -n "Checking for hard-coded config directory "
    inputFile=input/$modality/enroll_1N_short.txt
	tempConfigDir=$(shuf -er -n20  {A..Z} {a..z} {0..9} | tr -d '\n')
    chmod 775 $configDir; mv $configDir $tempConfigDir; chmod 550 $tempConfigDir
    bin/validate1N $modality enroll_1N -c $tempConfigDir -o $outputDir -h $outputStem -i $inputFile -t $numForks
    retEnrollment=$?
    if [[ $retEnrollment == 0 ]]; then
        echo "[SUCCESS]"
    elif [[ $retEnrollment == 2 ]]; then
        echo "[NOT IMPLEMENTED]"
        rm -rf $outputDir
        chmod 775 $tempConfigDir; mv $tempConfigDir $configDir; chmod 550 $configDir
        continue
    else
        chmod 775 $tempConfigDir
        mv $tempConfigDir $configDir
        echo "[ERROR] Detection of hard-coded config directory in your software.  Please fix!"
        exit $failure
    fi
    rm -rf $outputDir; mkdir -p $enrollDir
    chmod 775 $tempConfigDir; mv $tempConfigDir $configDir; chmod 550 $configDir

    if [ "$modality" == "face" ]; then
        echo -n "Running Enrollment on Multiple Images per Subject (Single Process) "
        inputFile=input/$modality/enroll_1N_multiface.txt
        bin/validate1N $modality enroll_1N -c $configDir -o $outputDir -h $outputStem -i $inputFile -t $numForks
        retEnrollment=$?
        if [[ $retEnrollment == 0 ]]; then
            echo "[SUCCESS]"
            # Merge output files together
            merge $outputDir/$outputStem enroll_1N
            # Merge edb and manifest together
            mergeEDB $outputDir
        elif [[ $retEnrollment == 2 ]]; then
            echo "[NOT IMPLEMENTED]"
            chmod -R 777 $outputDir; rm -rf $outputDir
            continue
        else
            echo "${bold}[ERROR] Enrollment validation (multiple images per subject) failed${normal}"
            exit $failure
        fi
        rm -rf $outputDir $enrollDir
        mkdir -p $enrollDir
    fi

    # Set number of child processes to fork()
    numForks=4
    echo -n "Running Enrollment (Multiple Processes) "
    # Enrollment
    inputFile=input/$modality/enroll_1N.txt
    bin/validate1N $modality enroll_1N -c $configDir -o $outputDir -h $outputStem -i $inputFile -t $numForks
    retEnrollment=$?
    if [[ $retEnrollment == 0 ]]; then
        echo "[SUCCESS]"
        # Merge output files together
        merge $outputDir/$outputStem enroll_1N
        # Merge edb and manifest together
        mergeEDB $outputDir
        # Change edb and manifest to READ-ONLY
        chmod -R 550 $outputDir/edb $outputDir/manifest
    elif [[ $retEnrollment == 2 ]]; then
        echo "[NOT IMPLEMENTED]"
        rm -rf $outputDir
        continue 
    else
        echo "${bold}[ERROR] Enrollment validation (multiple processes) failed.  Please ensure your software is compatible with fork(2).${normal}"
        exit $failure
    fi

    echo -n "Running Finalization "
    # Finalization
    bin/validate1N $modality finalize_1N -c $configDir -e $enrollDir -o $outputDir -h $outputStem
    retFinalize=$?
    if [[ $retFinalize == 0 ]]; then
        echo "[SUCCESS]"
    elif [[ $retFinalize == 2 ]]; then
        echo "[NOT IMPLEMENTED]"
        continue
    else
        echo "${bold}[ERROR] Finalize validation failed${normal}"
        exit $failure
    fi

    # Checking that the original EDB and manifest files are still there
    # after finalization.  Developers should NOT be modifying or moving
    # these files.
    if [ ! -s $outputDir/edb ] || [ ! -s $outputDir/manifest ]; then
        echo "${bold}[ERROR] $outputDir/edb and/or $outputDir/manifest are missing.  You should not be moving or altering these files!${normal}"
        exit $failure
    fi

    # Changing finalized enrollment directory to READ-ONLY.  Developers only have READ-ONLY access to the
    # enrollment directory during search.
    chmod -R 550 $enrollDir

    echo -n "Running Search (Multiple Processes) "
    # Search
    inputFile=input/$modality/search_1N.txt
    bin/validate1N $modality search_1N -c $configDir -e $enrollDir -o $outputDir -h $outputStem -i $inputFile -t $numForks
    retSearch=$?
    if [[ $retSearch == 0 ]]; then
        echo "[SUCCESS]"
        # Merge output files together
        merge $outputDir/$outputStem search_1N
    elif [[ $retSearch == 2 ]]; then
        echo "[NOT IMPLEMENTED]"
        continue
    else
        echo "${bold}[ERROR] Search (multiple processes) validation failed${normal}"
        exit $failure
    fi

    if [ "$modality" == "face" ]; then
        numForks=1
        echo -n "Running Search on Multi-person Templates (Single Process) "
        # Search
        inputFile=input/face/search_1N_multiperson.txt
        bin/validate1N $modality searchMulti_1N -c $configDir -e $enrollDir -o $outputDir -h $outputStem -i $inputFile -t $numForks
        retSearch=$?
        if [[ $retSearch == 0 ]]; then
            echo "[SUCCESS]"
            # Merge output files together
            merge $outputDir/$outputStem searchMulti_1N
        else
            echo "${bold}[ERROR] Search validation failed${normal}"
            exit $failure
        fi
    fi
    
    # If we get to the end, it means that validation occurred for
    # one modality.  We don't need to proceed with another modality.
    break 
done
