/*
  Software License :
  
  Copyright (c) 2003, The Open Effects Association Ltd. All rights reserved.
  
  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:
  
  * Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
  * Neither the name The Open Effects Association Ltd, nor the names of its 
  contributors may be used to endorse or promote products derived from this
  software without specific prior written permission.
  
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
  ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
  This is a wrapper around the openCV image segmentation function.
  This can be used for advanced keying or to create a "rotoscopic" look.
  
  The code is dervived from the openFX examples and has been written by Bernd Porr
  http://www.berndporr.me.uk, berndporr@f2s.com and has 
  the same license as above. The portions of the code written by
  Bernd Porr can be redistributed and re-used as long as proper credit is given.

  Status: beta
*/

static const char* pluginDescription =
  "Copyright (c) 2003, The Open Effects Association Ltd. All rights reserved.\n"
"\n"
"Redistribution and use in source and binary forms, with or without\n"
"modification, are permitted provided that the following conditions are met:\n"
"\n"
"    * Redistributions of source code must retain the above copyright notice,\n"
"      this list of conditions and the following disclaimer.\n"
"    * Redistributions in binary form must reproduce the above copyright notice,\n"
"      this list of conditions and the following disclaimer in the documentation\n"
"      and/or other materials provided with the distribution.\n"
"    * Neither the name The Open Effects Association Ltd, nor the names of its\n"
"      contributors may be used to endorse or promote products derived from this\n"
"      software without specific prior written permission.\n"
"\n"
"THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\" AND"
"ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED"
"WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE"
"DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR"
"ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES"
"(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;"
"LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON"
"ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT"
"(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS"
"SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.";


#include <string.h>
#include <math.h>
#include <stdio.h>
#include "cv.h"
#include "cvaux.h"
#include "highgui.h"
#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"
#include "ofxPixels.h"
#include "opencv2fx.h"

#if CV_MAJOR_VERSION >= 3
#error "This code won't work with OpenCV3 since it uses the - now gone for good - opencv-legacy module"
#endif

// pointers64 to various bits of the host
OfxHost                 *gHost;
OfxImageEffectSuiteV1 *gEffectHost = 0;
OfxPropertySuiteV1    *gPropHost = 0;
OfxParameterSuiteV1   *gParamHost = 0;
OfxMemorySuiteV1      *gMemoryHost = 0;
OfxMultiThreadSuiteV1 *gThreadHost = 0;
OfxMessageSuiteV1     *gMessageSuite = 0;
OfxInteractSuiteV1    *gInteractHost = 0;

#define THRESHOLD1 "threshold1"
#define THRESHOLD2 "threshold2"
#define BLOCK_SIZE 1000

// private instance data type
struct MyInstanceData {
  OfxParamHandle threshold1;
  OfxParamHandle threshold2;
  CvMemStorage* storage;
  CvSeq *comp;
};

// Convinience wrapper to get private data 
static MyInstanceData *
getMyInstanceData( OfxImageEffectHandle effect)
{
  // get the property handle for the plugin
  OfxPropertySetHandle effectProps = 0;
  OfxStatus stat;
  stat = gEffectHost->getPropertySet(effect, &effectProps);
  OFX::throwSuiteStatusException(stat);

  // get my data pointer out of that
  MyInstanceData *myData = 0;
  stat = gPropHost->propGetPointer(effectProps,
			    kOfxPropInstanceData, 
			    0, 
			    (void **) &myData);
  OFX::throwSuiteStatusException(stat);
  return myData;
}


//  instance construction
static OfxStatus
createInstance( OfxImageEffectHandle effect)
{
  // get a pointer to the effect properties
  OfxPropertySetHandle effectProps = 0;
  OfxStatus stat;
  stat = gEffectHost->getPropertySet(effect, &effectProps);
  OFX::throwSuiteStatusException(stat);

  // get a pointer to the effect's parameter set
  OfxParamSetHandle paramSet = 0;
  stat = gEffectHost->getParamSet(effect, &paramSet);
  OFX::throwSuiteStatusException(stat);

  // make my private instance data
  MyInstanceData *myData = new MyInstanceData;
  if (myData==NULL) return kOfxStatFailed;

  // cache away out param handles
  stat = gParamHost->paramGetHandle(paramSet, THRESHOLD1, &myData->threshold1, 0);
  OFX::throwSuiteStatusException(stat);
  stat = gParamHost->paramGetHandle(paramSet, THRESHOLD2, &myData->threshold2, 0);
  OFX::throwSuiteStatusException(stat);

  myData->storage = cvCreateMemStorage ( BLOCK_SIZE );
  myData->comp = NULL;

  // set my private instance data
  stat = gPropHost->propSetPointer(effectProps, kOfxPropInstanceData, 0, (void *) myData);
  OFX::throwSuiteStatusException(stat);

  return kOfxStatOK;
}


// instance destruction
static OfxStatus
destroyInstance( OfxImageEffectHandle  effect)
{
  // get my instance data
  MyInstanceData *myData = getMyInstanceData(effect);

  cvReleaseMemStorage(&(myData->storage));

  // and delete it
  if(myData)
    delete myData;

  return kOfxStatOK;
}




// look up a pixel in the image, does bounds checking to see if it is in the image rectangle
inline OfxRGBAColourB *
pixelAddress(OfxRGBAColourB *img, OfxRectI rect, int x, int y, int bytesPerLine)
{  
  if(x < rect.x1 || x >= rect.x2 || y < rect.y1 || y > rect.y2)
    return 0;
  OfxRGBAColourB *pix = (OfxRGBAColourB *) (((char *) img) + (y - rect.y1) * bytesPerLine);
  pix += x - rect.x1;  
  return pix;
}



// the process code  that the host sees
static OfxStatus render(OfxImageEffectHandle instance,
                        OfxPropertySetHandle inArgs,
                        OfxPropertySetHandle outArgs)
{
    // get the render window and the time from the inArgs
    OfxTime time;
    OfxRectI renderWindow;
    OfxStatus stat;

    stat = gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    OFX::throwSuiteStatusException(stat);
    stat = gPropHost->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &renderWindow.x1);
    OFX::throwSuiteStatusException(stat);

    // fetch output clip
    OfxImageClipHandle outputClip = 0;
    stat = gEffectHost->clipGetHandle(instance, kOfxImageEffectOutputClipName, &outputClip, 0);
    OFX::throwSuiteStatusException(stat);

    // fetch image to render into from that clip
    OfxPropertySetHandle outputImg = 0;
    stat = gEffectHost->clipGetImage(outputClip, time, NULL, &outputImg);
    OFX::throwSuiteStatusException(stat);

    // fetch output image info from that handle
    int dstRowBytes;
    OfxRectI dstRect;
    void *dstPtr;
    stat = gPropHost->propGetInt(outputImg, kOfxImagePropRowBytes, 0, &dstRowBytes);
    OFX::throwSuiteStatusException(stat);
    stat = gPropHost->propGetIntN(outputImg, kOfxImagePropBounds, 4, &dstRect.x1);
    OFX::throwSuiteStatusException(stat);
    stat = gPropHost->propGetInt(outputImg, kOfxImagePropRowBytes, 0, &dstRowBytes);
    OFX::throwSuiteStatusException(stat);
    stat = gPropHost->propGetPointer(outputImg, kOfxImagePropData, 0, &dstPtr);
    OFX::throwSuiteStatusException(stat);

    // fetch main input clip
    OfxImageClipHandle sourceClip = 0;
    stat = gEffectHost->clipGetHandle(instance, kOfxImageEffectSimpleSourceClipName, &sourceClip, 0);
    OFX::throwSuiteStatusException(stat);

    // fetch image at render time from that clip
    OfxPropertySetHandle sourceImg = 0;
    stat = gEffectHost->clipGetImage(sourceClip, time, NULL, &sourceImg);
    OFX::throwSuiteStatusException(stat);

    // fetch image info out of that handle
    int srcRowBytes;
    OfxRectI srcRect;
    void *srcPtr;
    stat = gPropHost->propGetInt(sourceImg, kOfxImagePropRowBytes, 0, &srcRowBytes);
    OFX::throwSuiteStatusException(stat);
    stat = gPropHost->propGetIntN(sourceImg, kOfxImagePropBounds, 4, &srcRect.x1);
    OFX::throwSuiteStatusException(stat);
    stat = gPropHost->propGetInt(sourceImg, kOfxImagePropRowBytes, 0, &srcRowBytes);
    OFX::throwSuiteStatusException(stat);
    stat = gPropHost->propGetPointer(sourceImg, kOfxImagePropData, 0, &srcPtr);
    OFX::throwSuiteStatusException(stat);

    // retrieve any instance data associated with this effect
    MyInstanceData *myData = getMyInstanceData(instance);
    
    double t1,t2;
    stat = gParamHost->paramGetValueAtTime(myData->threshold1, time, &t1);
    OFX::throwSuiteStatusException(stat);
    stat = gParamHost->paramGetValueAtTime(myData->threshold2, time, &t2);
    OFX::throwSuiteStatusException(stat);

    // cast data pointers to 8 bit RGBA
    OfxRGBAColourB *dst = (OfxRGBAColourB *) dstPtr;

    CvSize imageSize = cvSize(srcRect.x2-srcRect.x1, 
			      srcRect.y2-srcRect.y1);

    IplImage *imgSrc = cvCreateImageHeader(imageSize,
					   IPL_DEPTH_8U, 
					   4);

    imgSrc->imageData = (char*) srcPtr;
    imgSrc->widthStep = srcRowBytes;

    // levels of the pyramid
    int  level = 2;

    // we need to make the image smaller than it is
    imgSrc->width &= -(1<<level);
    imgSrc->height &= -(1<<level);

    CvSize reducedImageSize = cvSize(imgSrc->width,imgSrc->height);

    IplImage *image0 = cvCreateImage( reducedImageSize, IPL_DEPTH_8U, 3);
    IplImage *image1 = cvCreateImage( reducedImageSize, IPL_DEPTH_8U, 3);

    cvCvtColor( imgSrc , image0, CV_RGBA2RGB );
    cvCvtColor( imgSrc , image1, CV_RGBA2RGB );

    // perform the segmentation

    cvPyrSegmentation(image0, 
		      image1, 
		      myData->storage, 
		      &(myData->comp),
                      level, 
		      (int)t1, 
		      (int)t2);    


    // we need to be careful when writing back because the segmented image is smaller

    for(int y = renderWindow.y1; y < (renderWindow.y1 + image1->height); y++) {
        if(gEffectHost->abort(instance)) break;

	OfxRGBAColourB *dstPix = pixelAddress(dst, dstRect, renderWindow.x1, y, dstRowBytes);
	unsigned char *srcPix = (unsigned char*)(image1->imageData + y * image1->widthStep + renderWindow.x1);

        for(int x = renderWindow.x1; x < (renderWindow.x1 + image1->width); x++) {
        
                dstPix->r = srcPix[0];
                dstPix->g = srcPix[1];
                dstPix->b = srcPix[2];
                dstPix->a = 255;

	    dstPix++;
	    srcPix=srcPix+3;
        }
    }

    // just release the header but not the image itself. That will be done later.
    cvReleaseImageHeader(&imgSrc);
    cvReleaseImage(&image0);
    cvReleaseImage(&image1);
  
    // we are finished with the source images so release them
    stat = gEffectHost->clipReleaseImage(sourceImg);
    OFX::throwSuiteStatusException(stat);
    stat = gEffectHost->clipReleaseImage(outputImg);
    OFX::throwSuiteStatusException(stat);

    // all was well
    return kOfxStatOK;
}




//  describe the plugin in context
static OfxStatus
describeInContext( OfxImageEffectHandle  effect,  OfxPropertySetHandle inArgs)
{
  OfxPropertySetHandle props = 0;
  OfxStatus stat;
  // define the single output clip in both contexts
  stat = gEffectHost->clipDefine(effect, kOfxImageEffectOutputClipName, &props);
  OFX::throwSuiteStatusException(stat);

  // set the component types we can handle on out output
  stat = gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  OFX::throwSuiteStatusException(stat);

  // define the single source clip in both contexts
  stat = gEffectHost->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &props);
  OFX::throwSuiteStatusException(stat);

  // set the component types we can handle on our main input
  stat = gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  OFX::throwSuiteStatusException(stat);

  if(!gHost)
    return kOfxStatErrMissingHostFeature;
    
  gEffectHost   = (OfxImageEffectSuiteV1 *) gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
  gPropHost     = (OfxPropertySuiteV1 *)    gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
  gParamHost    = (OfxParameterSuiteV1 *)   gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
  gMemoryHost   = (OfxMemorySuiteV1 *)      gHost->fetchSuite(gHost->host, kOfxMemorySuite, 1);
  gThreadHost   = (OfxMultiThreadSuiteV1 *) gHost->fetchSuite(gHost->host, kOfxMultiThreadSuite, 1);
  gMessageSuite   = (OfxMessageSuiteV1 *)   gHost->fetchSuite(gHost->host, kOfxMessageSuite, 1);
  gInteractHost   = (OfxInteractSuiteV1 *)   gHost->fetchSuite(gHost->host, kOfxInteractSuite, 1);
  if(!gEffectHost || !gPropHost || !gParamHost || !gMemoryHost || !gThreadHost)
    return kOfxStatErrMissingHostFeature;

  ////////////////////////////////////////////////////////////////////////////////
  // define the parameters for this context
  // fetch the parameter set from the effect
  OfxParamSetHandle paramSet = 0;
  stat = gEffectHost->getParamSet(effect, &paramSet);
  OFX::throwSuiteStatusException(stat);

  defineDoubleParam(gPropHost,
		    gParamHost,
		    paramSet, 
		    THRESHOLD1, 
		    "threshold 1",
		    "Sets the threshold #1",
		    1,
		    255,
		    250);
  
  defineDoubleParam(gPropHost,
		    gParamHost,
		    paramSet, 
		    THRESHOLD2, 
		    "threshold 2",
		    "Sets the threshold #2",
		    1,
		    255,
		    30);

  // make a page of controls and add my parameters to it
  stat = gParamHost->paramDefine(paramSet, kOfxParamTypePage, "Main", &props);
  OFX::throwSuiteStatusException(stat);
  stat = gPropHost->propSetString(props, kOfxParamPropPageChild, 0, THRESHOLD1);
  OFX::throwSuiteStatusException(stat);
  stat = gPropHost->propSetString(props, kOfxParamPropPageChild, 1, THRESHOLD2);
  OFX::throwSuiteStatusException(stat);

  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// the plugin's description routine
static OfxStatus
describe(OfxImageEffectHandle effect)
{
  // get the property handle for the plugin
  OfxPropertySetHandle effectProps = 0;
  OfxStatus stat;
  stat = gEffectHost->getPropertySet(effect, &effectProps);
  OFX::throwSuiteStatusException(stat);

  // say we cannot support multiple pixel depths and let the clip preferences action deal with it all.
  stat = gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
  OFX::throwSuiteStatusException(stat);

  // set the bit depths the plugin can handle
  stat = gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
  OFX::throwSuiteStatusException(stat);

  // set plugin label and the group it belongs to
  stat = gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "openCV Segment");
  OFX::throwSuiteStatusException(stat);
  stat = gPropHost->propSetString(effectProps, kOfxImageEffectPluginPropGrouping, 0, PLUGIN_GROUPING);
  OFX::throwSuiteStatusException(stat);
  stat = gPropHost->propSetString(effectProps, kOfxPropPluginDescription, 0, pluginDescription);
  OFX::throwSuiteStatusException(stat);

  // define the contexts we can be used in
  stat = gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
  OFX::throwSuiteStatusException(stat);

  // set a few flags
  stat = gPropHost->propSetInt(effectProps, kOfxImageEffectPluginPropSingleInstance, 0, int(false));
  OFX::throwSuiteStatusException(stat);
  stat = gPropHost->propSetInt(effectProps, kOfxImageEffectPluginPropHostFrameThreading, 0, int(false));
  OFX::throwSuiteStatusException(stat);
  stat = gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsMultiResolution, 0, int(false));
  OFX::throwSuiteStatusException(stat);
  stat = gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsTiles, 0, int(false));
  OFX::throwSuiteStatusException(stat);
  stat = gPropHost->propSetInt(effectProps, kOfxImageEffectPropTemporalClipAccess, 0, int(false));
  OFX::throwSuiteStatusException(stat);
  stat = gPropHost->propSetInt(effectProps, kOfxImageEffectPluginPropFieldRenderTwiceAlways, 0, int(true));
  OFX::throwSuiteStatusException(stat);
  stat = gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsMultipleClipPARs, 0, int(false));
  OFX::throwSuiteStatusException(stat);

  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// Called at load
static OfxStatus
onLoad(void)
{
    // fetch the host suites out of the global host pointer
    if(!gHost) return kOfxStatErrMissingHostFeature;
    
    gEffectHost     = (OfxImageEffectSuiteV1 *) gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
    gPropHost       = (OfxPropertySuiteV1 *)    gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
    if(!gEffectHost || !gPropHost)
        return kOfxStatErrMissingHostFeature;
    return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// The main entry point function
static OfxStatus
pluginMain(const char *action,  const void *handle, OfxPropertySetHandle inArgs,  OfxPropertySetHandle outArgs)
{
    try {
        // cast to appropriate type
        OfxImageEffectHandle effect = (OfxImageEffectHandle) handle;

        if(strcmp(action, kOfxActionLoad) == 0) {
            return onLoad();
        } else if(strcmp(action, kOfxActionDescribe) == 0) {
            return describe(effect);
        } else if(strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
            return describeInContext(effect, inArgs);
        } else if(strcmp(action, kOfxImageEffectActionRender) == 0) {
            return render(effect, inArgs, outArgs);
        } else if(strcmp(action, kOfxActionCreateInstance) == 0) {
            return createInstance(effect);
        } else if(strcmp(action, kOfxActionDestroyInstance) == 0) {
            return destroyInstance(effect);
        }
    } catch (const OFX::Exception::Suite &e) {
        std::cout << "OFX Plugin Suite error: " << e.what() << std::endl;
        return e.status();
    } catch (std::bad_alloc) {
        // catch memory
        std::cout << "OFX Plugin Memory error." << std::endl;
        return kOfxStatErrMemory;
    } catch ( const std::exception& e ) {
        // standard exceptions
        std::cout << "OFX Plugin error: " << e.what() << std::endl;
        return kOfxStatErrUnknown;
    } catch ( ... ) {
        // everything else
        std::cout << "OFX Plugin error" << std::endl;
        return kOfxStatErrUnknown;
    }
    
    // other actions to take the default value
    return kOfxStatReplyDefault;
}

// function to set the host structure
static void
setHostFunc(OfxHost *hostStruct)
{
  gHost = hostStruct;
}

////////////////////////////////////////////////////////////////////////////////
// the plugin struct 
static OfxPlugin segmentPlugin = 
{       
  kOfxImageEffectPluginApi,
  1, // API version 1
  "uk.org.bratwurstandhaggis:cvPyrSegmentation",
  0, // major version
  5, // minor version
  setHostFunc, 
  pluginMain
};
   
// the two mandated functions
OfxPlugin *
OfxGetPlugin(int nth)
{
  if(nth == 0)
    return &segmentPlugin;
  return 0;
}
 
int
OfxGetNumberOfPlugins(void)
{       
  return 1;
}
