# This software was developed at the National Institute of Standards and
# Technology (NIST) by employees of the Federal Government in the course
# of their official duties. Pursuant to title 17 Section 105 of the
# United States Code, this software is not subject to copyright protection
# and is in the public domain. NIST assumes no responsibility whatsoever for
# its use by other parties, and makes no guarantees, expressed or implied,
# about its quality, reliability, or any other characteristic.

import cv2
import os
import numpy as np

def do_smad(image_array):
    #try:
        #print(f"DEBUG: Saving image with shape {image_array.shape}", flush=True)
        
        # NIST images are RGB, but OpenCV expects BGR for imwrite
        #image_bgr = cv2.cvtColor(image_array, cv2.COLOR_RGB2BGR)
        #filename = f"debug_image_{os.getpid()}.png"
        #success = cv2.imwrite(filename, image_bgr)
        
        #if success:
        #    print(f"DEBUG: Successfully saved {filename}", flush=True)
        #else:
        #    print(f"DEBUG: Failed to write {filename}. Check permissions.", flush=True)
            
    #except Exception as e:
        #print(f"DEBUG: Error during disk write: {str(e)}", flush=True)

    return 0.88;
