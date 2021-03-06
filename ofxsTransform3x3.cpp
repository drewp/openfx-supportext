/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-supportext <https://github.com/devernay/openfx-supportext>,
 * Copyright (C) 2015 INRIA
 *
 * openfx-supportext is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * openfx-supportext is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-supportext.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

/*
 * OFX Transform3x3 plugin: a base plugin for 2D homographic transform,
 * represented by a 3x3 matrix.
 */

/*
 Although the indications from nuke/fnOfxExtensions.h were followed, and the
 kFnOfxImageEffectActionGetTransform action was implemented in the Support
 library, that action is never called by the Nuke host.

 The extension was implemented as specified in Natron and in the Support library.
 
 @see gHostDescription.canTransform, ImageEffectDescriptor::setCanTransform(),
 and ImageEffect::getTransform().

 There is also an open question about how the last plugin in a transform chain
 may get the concatenated transform from upstream, the untransformed source image,
 concatenate its own transform and apply the resulting transform in its render
 action.
 
 Our solution is to have kFnOfxImageEffectCanTransform set on source clips for which
 a transform can be attached to fetched images.
 @see ClipDescriptor::setCanTransform().

 In this case, images fetched from the host may have a kFnOfxPropMatrix2D attached,
 which must be combined with the transformation applied by the effect (which
 may be any deformation function, not only a homography).
 @see ImageBase::getTransform() and ImageBase::getTransformIsIdentity
 */
// Uncomment the following to enable the experimental host transform code.
#define ENABLE_HOST_TRANSFORM

#include <memory>
#include <algorithm>

#include "ofxsTransform3x3.h"
#include "ofxsTransform3x3Processor.h"
#include "ofxsCoords.h"
#include "ofxsShutter.h"


#ifndef ENABLE_HOST_TRANSFORM
#undef OFX_EXTENSIONS_NUKE // host transform is the only nuke extension used
#endif

#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h"
#endif

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kRenderThreadSafety eRenderFullySafe

using namespace OFX;

// It would be nice to be able to cache the set of transforms (with motion blur) used to compute the
// current frame between two renders.
// Unfortunately, we cannot rely on the host sending changedParam() when the animation changes
// (Nuke doesn't call the action when a linked animation is changed),
// nor on dst->getUniqueIdentifier (which is "ffffffffffffffff" on Nuke)

#define kTransform3x3MotionBlurCount 1000 // number of transforms used in the motion

Transform3x3Plugin::Transform3x3Plugin(OfxImageEffectHandle handle,
                                       bool masked,
                                       Transform3x3ParamsTypeEnum paramsType)
    : ImageEffect(handle)
      , _dstClip(0)
      , _srcClip(0)
      , _maskClip(0)
      , _invert(0)
      , _filter(0)
      , _clamp(0)
      , _blackOutside(0)
      , _motionblur(0)
      , _amount(0)
      , _centered(0)
      , _fading(0)
      , _directionalBlur(0)
      , _shutter(0)
      , _shutteroffset(0)
      , _shuttercustomoffset(0)
      , _masked(masked)
      , _mix(0)
      , _maskApply(0)
      , _maskInvert(0)
{
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    assert(1 <= _dstClip->getPixelComponentCount() && _dstClip->getPixelComponentCount() <= 4);
    _srcClip = getContext() == OFX::eContextGenerator ? NULL : fetchClip(kOfxImageEffectSimpleSourceClipName);
    assert(!_srcClip || (1 <= _srcClip->getPixelComponentCount() && _srcClip->getPixelComponentCount() <= 4));
    // name of mask clip depends on the context
    if (masked) {
        _maskClip = fetchClip(getContext() == OFX::eContextPaint ? "Brush" : "Mask");
        assert(!_maskClip || _maskClip->getPixelComponents() == ePixelComponentAlpha);
    }

    if (paramExists(kParamTransform3x3Invert)) {
        // Transform3x3-GENERIC
        _invert = fetchBooleanParam(kParamTransform3x3Invert);
        // GENERIC
        _filter = fetchChoiceParam(kParamFilterType);
        _clamp = fetchBooleanParam(kParamFilterClamp);
        _blackOutside = fetchBooleanParam(kParamFilterBlackOutside);
        assert(_invert && _filter && _clamp && _blackOutside);
        if (paramExists(kParamTransform3x3MotionBlur)) {
            _motionblur = fetchDoubleParam(kParamTransform3x3MotionBlur); // GodRays may not have have _motionblur
            assert(_motionblur);
        }
        if (paramsType == eTransform3x3ParamsTypeDirBlur) {
            _amount = fetchDoubleParam(kParamTransform3x3Amount);
            _centered = fetchBooleanParam(kParamTransform3x3Centered);
            _fading = fetchDoubleParam(kParamTransform3x3Fading);
        } else if (paramsType == eTransform3x3ParamsTypeMotionBlur) {
            _directionalBlur = fetchBooleanParam(kParamTransform3x3DirectionalBlur);
            _shutter = fetchDoubleParam(kParamShutter);
            _shutteroffset = fetchChoiceParam(kParamShutterOffset);
            _shuttercustomoffset = fetchDoubleParam(kParamShutterCustomOffset);
            assert(_directionalBlur && _shutter && _shutteroffset && _shuttercustomoffset);
        }
        if (masked) {
            _mix = fetchDoubleParam(kParamMix);
            _maskInvert = fetchBooleanParam(kParamMaskInvert);
            assert(_mix && _maskInvert);
        }

        if (paramsType == eTransform3x3ParamsTypeMotionBlur) {
            bool directionalBlur;
            _directionalBlur->getValue(directionalBlur);
            _shutter->setEnabled(!directionalBlur);
            _shutteroffset->setEnabled(!directionalBlur);
            _shuttercustomoffset->setEnabled(!directionalBlur);
        }
    }
}

Transform3x3Plugin::~Transform3x3Plugin()
{
}

////////////////////////////////////////////////////////////////////////////////
/** @brief render for the filter */

////////////////////////////////////////////////////////////////////////////////
// basic plugin render function, just a skelington to instantiate templates from

/* set up and run a processor */
void
Transform3x3Plugin::setupAndProcess(Transform3x3ProcessorBase &processor,
                                    const OFX::RenderArguments &args)
{
    assert(!_invert || _motionblur); // this method should be overridden in GodRays
    const double time = args.time;
    std::auto_ptr<OFX::Image> dst( _dstClip->fetchImage(time) );

    if ( !dst.get() ) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }
    OFX::BitDepthEnum         dstBitDepth    = dst->getPixelDepth();
    OFX::PixelComponentEnum   dstComponents  = dst->getPixelComponents();
    if (dstBitDepth != _dstClip->getPixelDepth() ||
        dstComponents != _dstClip->getPixelComponents()) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong depth or components");
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }
    if ( (dst->getRenderScale().x != args.renderScale.x) ||
         ( dst->getRenderScale().y != args.renderScale.y) ||
         ( (dst->getField() != OFX::eFieldNone /* for DaVinci Resolve */ && dst->getField() != args.fieldToRender)) ) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }
    std::auto_ptr<const OFX::Image> src((_srcClip && _srcClip->isConnected()) ?
                                        _srcClip->fetchImage(args.time) : 0);
    size_t invtransformsizealloc = 0;
    size_t invtransformsize = 0;
    std::vector<OFX::Matrix3x3> invtransform;
    std::vector<double> invtransformalpha;
    double motionblur = 0.;
    bool directionalBlur = (_directionalBlur == 0);
    double amountFrom = 0.;
    double amountTo = 1.;
    if (_amount) {
        _amount->getValueAtTime(time, amountTo);
    }
    if (_centered) {
        bool centered;
        _centered->getValueAtTime(time, centered);
        if (centered) {
            amountFrom = -amountTo;
        }
    }
    bool blackOutside = false;
    double mix = 1.;

    if ( !src.get() ) {
        // no source image, use a dummy transform
        invtransformsizealloc = 1;
        invtransform.resize(invtransformsizealloc);
        invtransformsize = 1;
        invtransform[0].a = 0.;
        invtransform[0].b = 0.;
        invtransform[0].c = 0.;
        invtransform[0].d = 0.;
        invtransform[0].e = 0.;
        invtransform[0].f = 0.;
        invtransform[0].g = 0.;
        invtransform[0].h = 0.;
        invtransform[0].i = 1.;
    } else {
        OFX::BitDepthEnum dstBitDepth       = dst->getPixelDepth();
        OFX::PixelComponentEnum dstComponents  = dst->getPixelComponents();
        OFX::BitDepthEnum srcBitDepth      = src->getPixelDepth();
        OFX::PixelComponentEnum srcComponents = src->getPixelComponents();
        if ( (srcBitDepth != dstBitDepth) || (srcComponents != dstComponents) ) {
            OFX::throwSuiteStatusException(kOfxStatFailed);
            return;
        }

        bool invert = false;
        if (_invert) {
            _invert->getValueAtTime(time, invert);
        }

        if (_blackOutside) {
            _blackOutside->getValueAtTime(time, blackOutside);
        }
        if (_masked && _mix) {
            _mix->getValueAtTime(time, mix);
        }
        if (_motionblur) {
            _motionblur->getValueAtTime(time, motionblur);
        }
        if (_directionalBlur) {
            _directionalBlur->getValueAtTime(time, directionalBlur);
        }
        double shutter = 0.;
        if (!directionalBlur) {
            if (_shutter) {
                _shutter->getValueAtTime(time, shutter);
            }
        }
        const bool fielded = args.fieldToRender == OFX::eFieldLower || args.fieldToRender == OFX::eFieldUpper;
        const double pixelAspectRatio = src->getPixelAspectRatio();

        if ( (shutter != 0.) && (motionblur != 0.) ) {
            invtransformsizealloc = kTransform3x3MotionBlurCount;
            invtransform.resize(invtransformsizealloc);
            int shutteroffset_i;
            assert(_shutteroffset);
            _shutteroffset->getValueAtTime(time, shutteroffset_i);
            double shuttercustomoffset;
            assert(_shuttercustomoffset);
            _shuttercustomoffset->getValueAtTime(time, shuttercustomoffset);

            invtransformsize = getInverseTransforms(time, args.renderScale, fielded, pixelAspectRatio, invert, shutter, (ShutterOffsetEnum)shutteroffset_i, shuttercustomoffset, &invtransform.front(), invtransformsizealloc);
        } else if (directionalBlur) {
            invtransformsizealloc = kTransform3x3MotionBlurCount;
            invtransform.resize(invtransformsizealloc);
            invtransformalpha.resize(invtransformsizealloc);
            invtransformsize = getInverseTransformsBlur(time, args.renderScale, fielded, pixelAspectRatio, invert, amountFrom, amountTo, &invtransform.front(), &invtransformalpha.front(), invtransformsizealloc);
            // normalize alpha, and apply gamma
            double fading = 0.;
            if (_fading) {
                _fading->getValueAtTime(time, fading);
            }
            if (fading <= 0.) {
                std::fill(invtransformalpha.begin(), invtransformalpha.end(), 1.);
            } else {
                for (size_t i = 0; i < invtransformalpha.size(); ++i) {
                    invtransformalpha[i] = std::pow(1. - std::abs(invtransformalpha[i])/amountTo, fading);
                }
            }
        } else {
            invtransformsizealloc = 1;
            invtransform.resize(invtransformsizealloc);
            invtransformsize = 1;
            bool success = getInverseTransformCanonical(time, 1., invert, &invtransform[0]); // virtual function
            if (!success) {
                invtransform[0].a = 0.;
                invtransform[0].b = 0.;
                invtransform[0].c = 0.;
                invtransform[0].d = 0.;
                invtransform[0].e = 0.;
                invtransform[0].f = 0.;
                invtransform[0].g = 0.;
                invtransform[0].h = 0.;
                invtransform[0].i = 1.;
            } else {
                OFX::Matrix3x3 canonicalToPixel = OFX::ofxsMatCanonicalToPixel(pixelAspectRatio, args.renderScale.x,
                                                                               args.renderScale.y, fielded);
                OFX::Matrix3x3 pixelToCanonical = OFX::ofxsMatPixelToCanonical(pixelAspectRatio,  args.renderScale.x,
                                                                               args.renderScale.y, fielded);
                invtransform[0] = canonicalToPixel * invtransform[0] * pixelToCanonical;
            }
        }
        if (invtransformsize == 1) {
            motionblur  = 0.;
        }
        // compose with the input transform
        if ( !src->getTransformIsIdentity() ) {
            double srcTransform[9]; // transform to apply to the source image, in pixel coordinates, from source to destination
            src->getTransform(srcTransform);
            OFX::Matrix3x3 srcTransformMat;
            srcTransformMat.a = srcTransform[0];
            srcTransformMat.b = srcTransform[1];
            srcTransformMat.c = srcTransform[2];
            srcTransformMat.d = srcTransform[3];
            srcTransformMat.e = srcTransform[4];
            srcTransformMat.f = srcTransform[5];
            srcTransformMat.g = srcTransform[6];
            srcTransformMat.h = srcTransform[7];
            srcTransformMat.i = srcTransform[8];
            // invert it
            double det = ofxsMatDeterminant(srcTransformMat);
            if (det != 0.) {
                OFX::Matrix3x3 srcTransformInverse = ofxsMatInverse(srcTransformMat, det);

                for (size_t i = 0; i < invtransformsize; ++i) {
                    invtransform[i] = srcTransformInverse * invtransform[i];
                }
            }
        }
    }

    // auto ptr for the mask.
    bool doMasking = (_masked && (!_maskApply || _maskApply->getValueAtTime(args.time)) && _maskClip && _maskClip->isConnected());
    std::auto_ptr<const OFX::Image> mask(doMasking ? _maskClip->fetchImage(args.time) : 0);
    if (doMasking) {
        bool maskInvert = false;
        if (_maskInvert) {
            _maskInvert->getValueAtTime(time, maskInvert);
        }
        // say we are masking
        processor.doMasking(true);

        // Set it in the processor
        processor.setMaskImg(mask.get(), maskInvert);
    }

    // set the images
    processor.setDstImg( dst.get() );
    processor.setSrcImg( src.get() );

    // set the render window
    processor.setRenderWindow(args.renderWindow);
    assert(invtransform.size() && invtransformsize);
    processor.setValues(&invtransform.front(),
                        invtransformalpha.empty() ? 0 : &invtransformalpha.front(),
                        invtransformsize,
                        blackOutside,
                        motionblur,
                        mix);

    // Call the base class process member, this will call the derived templated process code
    processor.process();
} // setupAndProcess

// compute the bounding box of the transform of four points
static void
ofxsTransformRegionFromPoints(const OFX::Point3D p[4],
                              OfxRectD &rod)
{
    // extract the x/y bounds
    double x1, y1, x2, y2;

    // if all z's have the same sign, we can compute a reasonable ROI, else we give the whole image (the line at infinity crosses the rectangle)
    bool allpositive = true;
    bool allnegative = true;

    for (int i = 0; i < 4; ++i) {
        allnegative = allnegative && (p[i].z < 0.);
        allpositive = allpositive && (p[i].z > 0.);
    }

    if (!allpositive && !allnegative) {
        // the line at infinity crosses the source RoD
        x1 = kOfxFlagInfiniteMin;
        x2 = kOfxFlagInfiniteMax;
        y1 = kOfxFlagInfiniteMin;
        y2 = kOfxFlagInfiniteMax;
    } else {
        OfxPointD q[4];
        for (int i = 0; i < 4; ++i) {
            q[i].x = p[i].x / p[i].z;
            q[i].y = p[i].y / p[i].z;
        }

        x1 = x2 = q[0].x;
        y1 = y2 = q[0].y;
        for (int i = 1; i < 4; ++i) {
            if (q[i].x < x1) {
                x1 = q[i].x;
            } else if (q[i].x > x2) {
                x2 = q[i].x;
            }
            if (q[i].y < y1) {
                y1 = q[i].y;
            } else if (q[i].y > y2) {
                y2 = q[i].y;
            }
        }
    }

    // GENERIC
    rod.x1 = x1;
    rod.x2 = x2;
    rod.y1 = y1;
    rod.y2 = y2;
    assert(rod.x1 <= rod.x2 && rod.y1 <= rod.y2);
} // ofxsTransformRegionFromPoints

// compute the bounding box of the transform of a rectangle
static void
ofxsTransformRegionFromRoD(const OfxRectD &srcRoD,
                           const OFX::Matrix3x3 &transform,
                           OFX::Point3D p[4],
                           OfxRectD &rod)
{
    /// now transform the 4 corners of the source clip to the output image
    p[0] = transform * OFX::Point3D(srcRoD.x1,srcRoD.y1,1);
    p[1] = transform * OFX::Point3D(srcRoD.x1,srcRoD.y2,1);
    p[2] = transform * OFX::Point3D(srcRoD.x2,srcRoD.y2,1);
    p[3] = transform * OFX::Point3D(srcRoD.x2,srcRoD.y1,1);

    ofxsTransformRegionFromPoints(p, rod);
}

void
Transform3x3Plugin::transformRegion(const OfxRectD &rectFrom,
                                    double time,
                                    bool invert,
                                    double motionblur,
                                    bool directionalBlur,
                                    double amountFrom,
                                    double amountTo,
                                    double shutter,
                                    int shutteroffset_i,
                                    double shuttercustomoffset,
                                    bool isIdentity,
                                    OfxRectD *rectTo)
{
    // Algorithm:
    // - Compute positions of the four corners at start and end of shutter, and every multiple of 0.25 within this range.
    // - Update the bounding box from these positions.
    // - At the end, expand the bounding box by the maximum L-infinity distance between consecutive positions of each corner.

    OfxRangeD range;
    bool hasmotionblur = ((shutter != 0. || directionalBlur) && motionblur != 0.);

    if (hasmotionblur && !directionalBlur) {
        OFX::shutterRange(time, shutter, (ShutterOffsetEnum)shutteroffset_i, shuttercustomoffset, &range);
    } else {
        ///if is identity return the input rod instead of transforming
        if (isIdentity) {
            *rectTo = rectFrom;

            return;
        }
        range.min = range.max = time;
    }

    // initialize with a super-empty RoD (note that max and min are reversed)
    rectTo->x1 = kOfxFlagInfiniteMax;
    rectTo->x2 = kOfxFlagInfiniteMin;
    rectTo->y1 = kOfxFlagInfiniteMax;
    rectTo->y2 = kOfxFlagInfiniteMin;
    double t = range.min;
    bool first = true;
    bool last = !hasmotionblur; // ony one iteration if there is no motion blur
    bool finished = false;
    double expand = 0.;
    double amount = 1.;
    int dirBlurIter = 0;
    OFX::Point3D p_prev[4];
    while (!finished) {
        // compute transformed positions
        OfxRectD thisRoD;
        OFX::Matrix3x3 transform;
        bool success = getInverseTransformCanonical(t, amountFrom + amount * (amountTo - amountFrom), invert, &transform); // RoD is computed using the *DIRECT* transform, which is why we use !invert
        if (!success) {
            // return infinite region
            rectTo->x1 = kOfxFlagInfiniteMin;
            rectTo->x2 = kOfxFlagInfiniteMax;
            rectTo->y1 = kOfxFlagInfiniteMin;
            rectTo->y2 = kOfxFlagInfiniteMax;

            return;
        }
        OFX::Point3D p[4];
        ofxsTransformRegionFromRoD(rectFrom, transform, p, thisRoD);

        // update min/max
        OFX::Coords::rectBoundingBox(*rectTo, thisRoD, rectTo);

        // if first iteration, continue
        if (first) {
            first = false;
        } else {
            // compute the L-infinity distance between consecutive tested points
            expand = std::max( expand, std::fabs(p_prev[0].x - p[0].x) );
            expand = std::max( expand, std::fabs(p_prev[0].y - p[0].y) );
            expand = std::max( expand, std::fabs(p_prev[1].x - p[1].x) );
            expand = std::max( expand, std::fabs(p_prev[1].y - p[1].y) );
            expand = std::max( expand, std::fabs(p_prev[2].x - p[2].x) );
            expand = std::max( expand, std::fabs(p_prev[2].y - p[2].y) );
            expand = std::max( expand, std::fabs(p_prev[3].x - p[3].x) );
            expand = std::max( expand, std::fabs(p_prev[3].y - p[3].y) );
        }

        if (last) {
            finished = true;
        } else {
            // prepare for next iteration
            p_prev[0] = p[0];
            p_prev[1] = p[1];
            p_prev[2] = p[2];
            p_prev[3] = p[3];
            if (directionalBlur) {
                const int dirBlurIterMax = 8;
                ++ dirBlurIter;
                amount = 1. - dirBlurIter/(double)dirBlurIterMax;
                last = dirBlurIter == dirBlurIterMax;
            } else {
                t = std::floor(t * 4 + 1) / 4; // next quarter-frame
                if (t >= range.max) {
                    // last iteration should be done with range.max
                    t = range.max;
                    last = true;
                }
            }
        }
    }
    // expand to take into account errors due to motion blur
    if (rectTo->x1 > kOfxFlagInfiniteMin) {
        rectTo->x1 -= expand;
    }
    if (rectTo->x2 < kOfxFlagInfiniteMax) {
        rectTo->x2 += expand;
    }
    if (rectTo->y1 > kOfxFlagInfiniteMin) {
        rectTo->y1 -= expand;
    }
    if (rectTo->y2 < kOfxFlagInfiniteMax) {
        rectTo->y2 += expand;
    }
} // transformRegion

// override the rod call
// Transform3x3-GENERIC
// the RoD should at least contain the region of definition of the source clip,
// which will be filled with black or by continuity.
bool
Transform3x3Plugin::getRegionOfDefinition(const RegionOfDefinitionArguments &args,
                                          OfxRectD &rod)
{
    if (!_srcClip) {
        return false;
    }
    const double time = args.time;
    const OfxRectD& srcRoD = _srcClip->getRegionOfDefinition(time);

    if ( OFX::Coords::rectIsInfinite(srcRoD) ) {
        // return an infinite RoD
        rod.x1 = kOfxFlagInfiniteMin;
        rod.x2 = kOfxFlagInfiniteMax;
        rod.y1 = kOfxFlagInfiniteMin;
        rod.y2 = kOfxFlagInfiniteMax;

        return true;
    }

    double mix = 1.;
    bool doMasking = ((!_maskApply || _maskApply->getValueAtTime(args.time)) && _maskClip && _maskClip->isConnected());
    if (doMasking && _mix) {
        _mix->getValueAtTime(time, mix);
        if (mix == 0.) {
            // identity transform
            rod = srcRoD;

            return true;
        }
    }

    bool invert = false;
    if (_invert) {
        _invert->getValueAtTime(time, invert);
    }
    invert = !invert; // only for getRegionOfDefinition
    double motionblur = 1.; // default for GodRays
    if (_motionblur) {
        _motionblur->getValueAtTime(time, motionblur);
    }
    bool directionalBlur = (_directionalBlur == 0);
    double amountFrom = 0.;
    double amountTo = 1.;
    if (_amount) {
        _amount->getValueAtTime(time, amountTo);
    }
    if (_centered) {
        bool centered;
        _centered->getValueAtTime(time, centered);
        if (centered) {
            amountFrom = -amountTo;
        }
    }
    double shutter = 0.;
    int shutteroffset_i = 0;
    double shuttercustomoffset = 0.;
    if (_directionalBlur) {
        _directionalBlur->getValueAtTime(time, directionalBlur);
        _shutter->getValueAtTime(time, shutter);
        _shutteroffset->getValueAtTime(time, shutteroffset_i);
        _shuttercustomoffset->getValueAtTime(time, shuttercustomoffset);
    }

    bool identity = isIdentity(args.time);

    // set rod from srcRoD
    transformRegion(srcRoD, time, invert, motionblur, directionalBlur, amountFrom, amountTo, shutter, shutteroffset_i, shuttercustomoffset, identity, &rod);

    // If identity do not expand for black outside, otherwise we would never be able to have identity.
    // We want the RoD to be the same as the src RoD when we are identity.
    if (!identity) {
        bool blackOutside = false;
        if (_blackOutside) {
            _blackOutside->getValueAtTime(time, blackOutside);
        }

        ofxsFilterExpandRoD(this, _dstClip->getPixelAspectRatio(), args.renderScale, blackOutside, &rod);
    }

    if ( doMasking && (mix != 1. || _maskClip->isConnected()) ) {
        // for masking or mixing, we also need the source image.
        // compute the union of both RODs
        OFX::Coords::rectBoundingBox(rod, srcRoD, &rod);
    }

    // say we set it
    return true;
} // getRegionOfDefinition

// override the roi call
// Transform3x3-GENERIC
// Required if the plugin requires a region from the inputs which is different from the rendered region of the output.
// (this is the case for transforms)
// It may be difficult to implement for complicated transforms:
// consequently, these transforms cannot support tiles.
void
Transform3x3Plugin::getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args,
                                         OFX::RegionOfInterestSetter &rois)
{
    if (!_srcClip) {
        return;
    }
    const double time = args.time;
    const OfxRectD roi = args.regionOfInterest;
    OfxRectD srcRoI;
    double mix = 1.;
    bool doMasking = ((!_maskApply || _maskApply->getValueAtTime(args.time)) && _maskClip && _maskClip->isConnected());
    if (doMasking) {
        _mix->getValueAtTime(time, mix);
        if (mix == 0.) {
            // identity transform
            srcRoI = roi;
            rois.setRegionOfInterest(*_srcClip, srcRoI);

            return;
        }
    }

    bool invert = false;
    if (_invert) {
        _invert->getValueAtTime(time, invert);
    }
    //invert = !invert; // only for getRegionOfDefinition
    double motionblur = 1; // default for GodRays
    if (_motionblur) {
        _motionblur->getValueAtTime(time, motionblur);
    }
    bool directionalBlur = (_directionalBlur == 0);
    double amountFrom = 0.;
    double amountTo = 1.;
    if (_amount) {
        _amount->getValueAtTime(time, amountTo);
    }
    if (_centered) {
        bool centered;
        _centered->getValueAtTime(time, centered);
        if (centered) {
            amountFrom = -amountTo;
        }
    }
    double shutter = 0.;
    int shutteroffset_i = 0;
    double shuttercustomoffset = 0.;
    if (_directionalBlur) {
        _directionalBlur->getValueAtTime(time, directionalBlur);
        _shutter->getValueAtTime(time, shutter);
        _shutteroffset->getValueAtTime(time, shutteroffset_i);
        _shuttercustomoffset->getValueAtTime(time, shuttercustomoffset);
    }
    // set srcRoI from roi
    transformRegion(roi, time, invert, motionblur, directionalBlur, amountFrom, amountTo, shutter, shutteroffset_i, shuttercustomoffset, isIdentity(time), &srcRoI);

    int filter = eFilterCubic;
    if (_filter) {
        _filter->getValueAtTime(time, filter);
    }
    bool blackOutside = false;
    if (_blackOutside) {
        _blackOutside->getValueAtTime(time, blackOutside);
    }

    assert(srcRoI.x1 <= srcRoI.x2 && srcRoI.y1 <= srcRoI.y2);

    ofxsFilterExpandRoI(roi, _srcClip->getPixelAspectRatio(), args.renderScale, (FilterEnum)filter, doMasking, mix, &srcRoI);

    if ( OFX::Coords::rectIsInfinite(srcRoI) ) {
        // RoI cannot be infinite.
        // This is not a mathematically correct solution, but better than nothing: set to the project size
        OfxPointD size = getProjectSize();
        OfxPointD offset = getProjectOffset();

        if (srcRoI.x1 <= kOfxFlagInfiniteMin) {
            srcRoI.x1 = offset.x;
        }
        if (srcRoI.x2 >= kOfxFlagInfiniteMax) {
            srcRoI.x2 = offset.x + size.x;
        }
        if (srcRoI.y1 <= kOfxFlagInfiniteMin) {
            srcRoI.y1 = offset.y;
        }
        if (srcRoI.y2 >= kOfxFlagInfiniteMax) {
            srcRoI.y2 = offset.y + size.y;
        }
    }

    if ( _masked && (mix != 1.) ) {
        // compute the bounding box with the default ROI
        OFX::Coords::rectBoundingBox(srcRoI, args.regionOfInterest, &srcRoI);
    }

    // no need to set it on mask (the default ROI is OK)
    rois.setRegionOfInterest(*_srcClip, srcRoI);
} // getRegionsOfInterest

template <class PIX, int nComponents, int maxValue, bool masked>
void
Transform3x3Plugin::renderInternalForBitDepth(const OFX::RenderArguments &args)
{
    const double time = args.time;
    int filter = args.renderQualityDraft ? eFilterImpulse : eFilterCubic;
    if (!args.renderQualityDraft && _filter) {
        _filter->getValueAtTime(time, filter);
    }
    bool clamp = false;
    if (_clamp) {
        _clamp->getValueAtTime(time, clamp);
    }

    // as you may see below, some filters don't need explicit clamping, since they are
    // "clamped" by construction.
    switch ( (FilterEnum)filter ) {
    case eFilterImpulse: {
        Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterImpulse, false> fred(*this);
        setupAndProcess(fred, args);
        break;
    }
    case eFilterBilinear: {
        Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterBilinear, false> fred(*this);
        setupAndProcess(fred, args);
        break;
    }
    case eFilterCubic: {
        Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterCubic, false> fred(*this);
        setupAndProcess(fred, args);
        break;
    }
    case eFilterKeys:
        if (clamp) {
            Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterKeys, true> fred(*this);
            setupAndProcess(fred, args);
        } else {
            Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterKeys, false> fred(*this);
            setupAndProcess(fred, args);
        }
        break;
    case eFilterSimon:
        if (clamp) {
            Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterSimon, true> fred(*this);
            setupAndProcess(fred, args);
        } else {
            Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterSimon, false> fred(*this);
            setupAndProcess(fred, args);
        }
        break;
    case eFilterRifman:
        if (clamp) {
            Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterRifman, true> fred(*this);
            setupAndProcess(fred, args);
        } else {
            Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterRifman, false> fred(*this);
            setupAndProcess(fred, args);
        }
        break;
    case eFilterMitchell:
        if (clamp) {
            Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterMitchell, true> fred(*this);
            setupAndProcess(fred, args);
        } else {
            Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterMitchell, false> fred(*this);
            setupAndProcess(fred, args);
        }
        break;
    case eFilterParzen: {
        Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterParzen, false> fred(*this);
        setupAndProcess(fred, args);
        break;
    }
    case eFilterNotch: {
        Transform3x3Processor<PIX, nComponents, maxValue, masked, eFilterNotch, false> fred(*this);
        setupAndProcess(fred, args);
        break;
    }
    } // switch
} // renderInternalForBitDepth

// the internal render function
template <int nComponents, bool masked>
void
Transform3x3Plugin::renderInternal(const OFX::RenderArguments &args,
                                   OFX::BitDepthEnum dstBitDepth)
{
    switch (dstBitDepth) {
    case OFX::eBitDepthUByte:
        renderInternalForBitDepth<unsigned char, nComponents, 255, masked>(args);
        break;
    case OFX::eBitDepthUShort:
        renderInternalForBitDepth<unsigned short, nComponents, 65535, masked>(args);
        break;
    case OFX::eBitDepthFloat:
        renderInternalForBitDepth<float, nComponents, 1, masked>(args);
        break;
    default:
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

// the overridden render function
void
Transform3x3Plugin::render(const OFX::RenderArguments &args)
{
    // instantiate the render code based on the pixel depth of the dst clip
    OFX::BitDepthEnum dstBitDepth    = _dstClip->getPixelDepth();
    int dstComponentCount  = _dstClip->getPixelComponentCount();

    assert(1 <= dstComponentCount && dstComponentCount <= 4);
    switch (dstComponentCount) {
        case 4:
            if (_masked) {
                renderInternal<4,true>(args, dstBitDepth);
            } else {
                renderInternal<4,false>(args, dstBitDepth);
            }
            break;
        case 3:
            if (_masked) {
                renderInternal<3,true>(args, dstBitDepth);
            } else {
                renderInternal<3,false>(args, dstBitDepth);
            }
            break;
        case 2:
            if (_masked) {
                renderInternal<2,true>(args, dstBitDepth);
            } else {
                renderInternal<2,false>(args, dstBitDepth);
            }
            break;
        case 1:
            if (_masked) {
                renderInternal<1,true>(args, dstBitDepth);
            } else {
                renderInternal<1,false>(args, dstBitDepth);
            }
            break;
        default:
            break;
    }
}

bool
Transform3x3Plugin::isIdentity(const IsIdentityArguments &args,
                               OFX::Clip * &identityClip,
                               double &identityTime)
{
    // must clear persistent message in isIdentity, or render() is not called by Nuke after an error
    clearPersistentMessage();

    const double time = args.time;

    if (_amount) {
        double amount = 1.;
        _amount->getValueAtTime(time, amount);
        if (amount == 0.) {
            identityClip = _srcClip;
            identityTime = time;

            return true;
        }
    }

    // if there is motion blur, we suppose the transform is not identity
    double motionblur = _invert ? 1. : 0.; // default is 1 for GodRays, 0 for Mirror
    if (_motionblur) {
        _motionblur->getValueAtTime(time, motionblur);
    }
    double shutter = 0.;
    if (_shutter) {
        _shutter->getValueAtTime(time, shutter);
    }
    bool hasmotionblur = (shutter != 0. && motionblur != 0.);
    if (hasmotionblur) {
        return false;
    }

    if (_clamp) {
        // if image has values above 1., they will be clamped.
        bool clamp;
        _clamp->getValueAtTime(time, clamp);
        if (clamp) {
            return false;
        }
    }

    if ( isIdentity(time) ) { // let's call the Transform-specific one first
        identityClip = _srcClip;
        identityTime = time;

        return true;
    }

    // GENERIC
    if (_masked) {
        double mix = 1.;
        if (_mix) {
            _mix->getValueAtTime(time, mix);
        }
        if (mix == 0.) {
            identityClip = _srcClip;
            identityTime = time;

            return true;
        }

        bool doMasking = ((!_maskApply || _maskApply->getValueAtTime(args.time)) && _maskClip && _maskClip->isConnected());
        if (doMasking) {
            bool maskInvert;
            _maskInvert->getValueAtTime(args.time, maskInvert);
            if (!maskInvert) {
                OfxRectI maskRoD;
                OFX::Coords::toPixelEnclosing(_maskClip->getRegionOfDefinition(args.time), args.renderScale, _maskClip->getPixelAspectRatio(), &maskRoD);
                // effect is identity if the renderWindow doesn't intersect the mask RoD
                if (!OFX::Coords::rectIntersection<OfxRectI>(args.renderWindow, maskRoD, 0)) {
                    identityClip = _srcClip;
                    return true;
                }
            }
        }
    }

    return false;
}

#ifdef OFX_EXTENSIONS_NUKE
// overridden getTransform
bool
Transform3x3Plugin::getTransform(const TransformArguments &args,
                                 Clip * &transformClip,
                                 double transformMatrix[9])
{
    assert(!_masked); // this should never get called for masked plugins, since they don't advertise that they can transform
    if (_masked) {
        return false;
    }
    const double time = args.time;
    bool invert = false;

    //std::cout << "getTransform called!" << std::endl;
    // Transform3x3-GENERIC
    if (_invert) {
        _invert->getValueAtTime(time, invert);
    }

    OFX::Matrix3x3 invtransform;
    bool success = getInverseTransformCanonical(time, 1., invert, &invtransform);
    if (!success) {
        return false;
    }


    // invert it
    double det = ofxsMatDeterminant(invtransform);
    if (det == 0.) {
        return false; // no transform available, render as usual
    }
    OFX::Matrix3x3 transformCanonical = ofxsMatInverse(invtransform, det);
    double pixelaspectratio = _srcClip ? _srcClip->getPixelAspectRatio() : 1.;
    bool fielded = args.fieldToRender == eFieldLower || args.fieldToRender == eFieldUpper;
    OFX::Matrix3x3 transformPixel = ( OFX::ofxsMatCanonicalToPixel(pixelaspectratio, args.renderScale.x, args.renderScale.y, fielded) *
                                      transformCanonical *
                                      OFX::ofxsMatPixelToCanonical(pixelaspectratio, args.renderScale.x, args.renderScale.y, fielded) );
    transformClip = _srcClip;
    transformMatrix[0] = transformPixel.a;
    transformMatrix[1] = transformPixel.b;
    transformMatrix[2] = transformPixel.c;
    transformMatrix[3] = transformPixel.d;
    transformMatrix[4] = transformPixel.e;
    transformMatrix[5] = transformPixel.f;
    transformMatrix[6] = transformPixel.g;
    transformMatrix[7] = transformPixel.h;
    transformMatrix[8] = transformPixel.i;

    return true;
}

#endif

size_t
Transform3x3Plugin::getInverseTransforms(double time,
                                         OfxPointD renderscale,
                                         bool fielded,
                                         double pixelaspectratio,
                                         bool invert,
                                         double shutter,
                                         ShutterOffsetEnum shutteroffset,
                                         double shuttercustomoffset,
                                         OFX::Matrix3x3* invtransform,
                                         size_t invtransformsizealloc) const
{
    OfxRangeD range;

    OFX::shutterRange(time, shutter, shutteroffset, shuttercustomoffset, &range);
    double t_start = range.min;
    double t_end = range.max; // shutter time
    bool allequal = true;
    size_t invtransformsize = invtransformsizealloc;
    OFX::Matrix3x3 canonicalToPixel = OFX::ofxsMatCanonicalToPixel(pixelaspectratio, renderscale.x, renderscale.y, fielded);
    OFX::Matrix3x3 pixelToCanonical = OFX::ofxsMatPixelToCanonical(pixelaspectratio, renderscale.x, renderscale.y, fielded);
    OFX::Matrix3x3 invtransformCanonical;

    for (size_t i = 0; i < invtransformsize; ++i) {
        double t = (i == 0) ? t_start : ( t_start + i * (t_end - t_start) / (double)(invtransformsizealloc - 1) );
        bool success = getInverseTransformCanonical(t, 1., invert, &invtransformCanonical); // virtual function
        if (success) {
            invtransform[i] = canonicalToPixel * invtransformCanonical * pixelToCanonical;
        } else {
            invtransform[i].a = 0.;
            invtransform[i].b = 0.;
            invtransform[i].c = 0.;
            invtransform[i].d = 0.;
            invtransform[i].e = 0.;
            invtransform[i].f = 0.;
            invtransform[i].g = 0.;
            invtransform[i].h = 0.;
            invtransform[i].i = 1.;
        }
        allequal = allequal && (invtransform[i].a == invtransform[0].a &&
                                invtransform[i].b == invtransform[0].b &&
                                invtransform[i].c == invtransform[0].c &&
                                invtransform[i].d == invtransform[0].d &&
                                invtransform[i].e == invtransform[0].e &&
                                invtransform[i].f == invtransform[0].f &&
                                invtransform[i].g == invtransform[0].g &&
                                invtransform[i].h == invtransform[0].h &&
                                invtransform[i].i == invtransform[0].i);
    }
    if (allequal) { // there is only one transform, no need to do motion blur!
        invtransformsize = 1;
    }

    return invtransformsize;
}

size_t
Transform3x3Plugin::getInverseTransformsBlur(double time,
                                             OfxPointD renderscale,
                                             bool fielded,
                                             double pixelaspectratio,
                                             bool invert,
                                             double amountFrom,
                                             double amountTo,
                                             OFX::Matrix3x3* invtransform,
                                             double *amount,
                                             size_t invtransformsizealloc) const
{
    bool allequal = true;
    OFX::Matrix3x3 canonicalToPixel = OFX::ofxsMatCanonicalToPixel(pixelaspectratio, renderscale.x, renderscale.y, fielded);
    OFX::Matrix3x3 pixelToCanonical = OFX::ofxsMatPixelToCanonical(pixelaspectratio, renderscale.x, renderscale.y, fielded);
    OFX::Matrix3x3 invtransformCanonical;

    size_t invtransformsize = 0;
    for (size_t i = 0; i < invtransformsizealloc; ++i) {
        //double a = 1. - i / (double)(invtransformsizealloc - 1); // Theoretically better
        double a = 1. - (i+1) / (double)(invtransformsizealloc); // To be compatible with Nuke (Nuke bug?)
        double amt = amountFrom + (amountTo - amountFrom) * a;
        bool success = getInverseTransformCanonical(time, amt, invert, &invtransformCanonical); // virtual function
        if (success) {
            if (amount) {
                amount[invtransformsize] = amt;
            }
            invtransform[invtransformsize] = canonicalToPixel * invtransformCanonical * pixelToCanonical;
            ++invtransformsize;
            allequal = allequal && (invtransform[i].a == invtransform[0].a &&
                                    invtransform[i].b == invtransform[0].b &&
                                    invtransform[i].c == invtransform[0].c &&
                                    invtransform[i].d == invtransform[0].d &&
                                    invtransform[i].e == invtransform[0].e &&
                                    invtransform[i].f == invtransform[0].f &&
                                    invtransform[i].g == invtransform[0].g &&
                                    invtransform[i].h == invtransform[0].h &&
                                    invtransform[i].i == invtransform[0].i);
        }
    }
    if (invtransformsize != 0 && allequal) { // there is only one transform, no need to do motion blur!
        invtransformsize = 1;
    }

    return invtransformsize;
}

// override changedParam
void
Transform3x3Plugin::changedParam(const OFX::InstanceChangedArgs &args,
                                 const std::string &paramName)
{
    if ( (paramName == kParamTransform3x3Invert) ||
         ( paramName == kParamShutter) ||
         ( paramName == kParamShutterOffset) ||
         ( paramName == kParamShutterCustomOffset) ) {
        // Motion Blur is the only parameter that doesn't matter
        assert(paramName != kParamTransform3x3MotionBlur);

        changedTransform(args);
    }
    if (paramName == kParamTransform3x3DirectionalBlur) {
        bool directionalBlur;
        _directionalBlur->getValueAtTime(args.time, directionalBlur);
        _shutter->setEnabled(!directionalBlur);
        _shutteroffset->setEnabled(!directionalBlur);
        _shuttercustomoffset->setEnabled(!directionalBlur);
    }
}

// this method must be called by the derived class when the transform was changed
void
Transform3x3Plugin::changedTransform(const OFX::InstanceChangedArgs &args)
{
    (void)args;
}

void
OFX::Transform3x3Describe(OFX::ImageEffectDescriptor &desc,
                          bool masked)
{
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    if (masked) {
        desc.addSupportedContext(eContextPaint);
    }
    desc.addSupportedBitDepth(eBitDepthUByte);
    desc.addSupportedBitDepth(eBitDepthUShort);
    desc.addSupportedBitDepth(eBitDepthFloat);

    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setTemporalClipAccess(false);
    // each field has to be transformed separately, or you will get combing effect
    // this should be true for all geometric transforms
    desc.setRenderTwiceAlways(true);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(kRenderThreadSafety);
    desc.setSupportsRenderQuality(true);

    // Transform3x3-GENERIC

    // in order to support tiles, the transform plugin must implement the getRegionOfInterest function
    desc.setSupportsTiles(kSupportsTiles);

    // in order to support multiresolution, render() must take into account the pixelaspectratio and the renderscale
    // and scale the transform appropriately.
    // All other functions are usually in canonical coordinates.
    desc.setSupportsMultiResolution(kSupportsMultiResolution);

#ifdef OFX_EXTENSIONS_NUKE
    if (!masked) {
        // Enable transform by the host.
        // It is only possible for transforms which can be represented as a 3x3 matrix.
        desc.setCanTransform(true);
        if (getImageEffectHostDescription()->canTransform) {
            //std::cout << "kFnOfxImageEffectCanTransform (describe) =" << desc.getPropertySet().propGetInt(kFnOfxImageEffectCanTransform) << std::endl;
        }
    }
    // ask the host to render all planes
    desc.setPassThroughForNotProcessedPlanes(ePassThroughLevelRenderAllRequestedPlanes);
#endif
#ifdef OFX_EXTENSIONS_NATRON
    desc.setChannelSelector(ePixelComponentNone);
#endif
}

OFX::PageParamDescriptor *
OFX::Transform3x3DescribeInContextBegin(OFX::ImageEffectDescriptor &desc,
                                        OFX::ContextEnum context,
                                        bool masked)
{
    // GENERIC

    // Source clip only in the filter context
    // create the mandated source clip
    // always declare the source clip first, because some hosts may consider
    // it as the default input clip (e.g. Nuke)
    ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);

    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->addSupportedComponent(ePixelComponentXY);
    srcClip->addSupportedComponent(ePixelComponentAlpha);
#ifdef OFX_EXTENSIONS_NATRON
    srcClip->addSupportedComponent(ePixelComponentXY);
#endif
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);
    srcClip->setCanTransform(true); // source images can have a transform attached

    if (masked) {
        // GENERIC (MASKED)
        //
        // if general or paint context, define the mask clip
        // if paint context, it is a mandated input called 'brush'
        ClipDescriptor *maskClip = (context == eContextPaint) ? desc.defineClip("Brush") : desc.defineClip("Mask");
        maskClip->addSupportedComponent(ePixelComponentAlpha);
        maskClip->setTemporalClipAccess(false);
        if (context == eContextGeneral) {
            maskClip->setOptional(true);
        }
        maskClip->setSupportsTiles(kSupportsTiles);
        maskClip->setIsMask(true); // we are a mask input
    }

    // create the mandated output clip
    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->addSupportedComponent(ePixelComponentXY);
    dstClip->addSupportedComponent(ePixelComponentAlpha);
#ifdef OFX_EXTENSIONS_NATRON
    dstClip->addSupportedComponent(ePixelComponentXY);
#endif
    dstClip->setSupportsTiles(kSupportsTiles);


    // make some pages and to things in
    PageParamDescriptor *page = desc.definePageParam("Controls");

    return page;
}

void
OFX::Transform3x3DescribeInContextEnd(OFX::ImageEffectDescriptor &desc,
                                      OFX::ContextEnum context,
                                      OFX::PageParamDescriptor* page,
                                      bool masked,
                                      OFX::Transform3x3Plugin::Transform3x3ParamsTypeEnum paramsType)
{
    // invert
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamTransform3x3Invert);
        param->setLabel(kParamTransform3x3InvertLabel);
        param->setHint(kParamTransform3x3InvertHint);
        param->setDefault(false);
        param->setAnimates(true);
        if (page) {
            page->addChild(*param);
        }
    }
    // GENERIC PARAMETERS
    //

    ofxsFilterDescribeParamsInterpolate2D(desc, page, paramsType == OFX::Transform3x3Plugin::eTransform3x3ParamsTypeMotionBlur);

    // motionBlur
    {
        DoubleParamDescriptor* param = desc.defineDoubleParam(kParamTransform3x3MotionBlur);
        param->setLabel(kParamTransform3x3MotionBlurLabel);
        param->setHint(kParamTransform3x3MotionBlurHint);
        param->setDefault(paramsType == OFX::Transform3x3Plugin::eTransform3x3ParamsTypeDirBlur ? 1. : 0.);
        param->setRange(0., 100.);
        param->setIncrement(0.01);
        param->setDisplayRange(0., 4.);
        if (page) {
            page->addChild(*param);
        }
    }

    if (paramsType == OFX::Transform3x3Plugin::eTransform3x3ParamsTypeDirBlur) {
        {
            DoubleParamDescriptor *param = desc.defineDoubleParam(kParamTransform3x3Amount);
            param->setLabel(kParamTransform3x3AmountLabel);
            param->setHint(kParamTransform3x3AmountHint);
            //param->setRange(-1, 2.);
            param->setDisplayRange(-1, 2.);
            param->setDefault(1);
            param->setAnimates(true); // can animate
            if (page) {
                page->addChild(*param);
            }
        }

        {
            BooleanParamDescriptor *param = desc.defineBooleanParam(kParamTransform3x3Centered);
            param->setLabel(kParamTransform3x3CenteredLabel);
            param->setHint(kParamTransform3x3CenteredHint);
            param->setAnimates(true); // can animate
            if (page) {
                page->addChild(*param);
            }
        }

        {
            DoubleParamDescriptor *param = desc.defineDoubleParam(kParamTransform3x3Fading);
            param->setLabel(kParamTransform3x3FadingLabel);
            param->setHint(kParamTransform3x3FadingHint);
            param->setRange(0., 4.);
            param->setDisplayRange(0., 4.);
            param->setDefault(0.);
            param->setAnimates(true); // can animate
            if (page) {
                page->addChild(*param);
            }
        }
    } else if (paramsType == OFX::Transform3x3Plugin::eTransform3x3ParamsTypeMotionBlur) {
        // directionalBlur
        {
            BooleanParamDescriptor* param = desc.defineBooleanParam(kParamTransform3x3DirectionalBlur);
            param->setLabel(kParamTransform3x3DirectionalBlurLabel);
            param->setHint(kParamTransform3x3DirectionalBlurHint);
            param->setDefault(false);
            param->setAnimates(true);
            if (page) {
                page->addChild(*param);
            }
        }

        shutterDescribeInContext(desc, context, page);
    }
    
    if (masked) {
        // GENERIC (MASKED)
        //
        ofxsMaskMixDescribeParams(desc, page);
#ifdef OFX_EXTENSIONS_NUKE
    } else if (getImageEffectHostDescription()->canTransform) {
        // Transform3x3-GENERIC (NON-MASKED)
        //
        //std::cout << "kFnOfxImageEffectCanTransform in describeincontext(" << context << ")=" << desc.getPropertySet().propGetInt(kFnOfxImageEffectCanTransform) << std::endl;
#endif
    }
} // Transform3x3DescribeInContextEnd

