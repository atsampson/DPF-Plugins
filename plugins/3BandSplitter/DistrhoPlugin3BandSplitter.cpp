/*
 * DISTRHO 3BandSplitter Plugin, based on 3BandSplitter by Michael Gruhn
 * Copyright (C) 2007 Michael Gruhn <michael-gruhn@web.de>
 * Copyright (C) 2012-2015 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * For a full copy of the license see the LICENSE file.
 */

#include "DistrhoPlugin3BandSplitter.hpp"

#include <cmath>

static const float kAMP_DB = 8.656170245f;
static const float kDC_ADD = 1e-30f;
static const float kPI     = 3.141592654f;

START_NAMESPACE_DISTRHO

// -----------------------------------------------------------------------

DistrhoPlugin3BandSplitter::DistrhoPlugin3BandSplitter()
    : Plugin(paramCount, 1, 0) // 1 program, 0 states
{
    // set default values
    loadProgram(0);

    // reset
    deactivate();
}

// -----------------------------------------------------------------------
// Init

void DistrhoPlugin3BandSplitter::initAudioPort(bool input, uint32_t index, AudioPort& port)
{
    port.hints = 0x0;

    if (input)
    {
        switch (index)
        {
        case 0:
            port.name   = "Inpput Left";
            port.symbol = "in_left";
            break;
        case 1:
            port.name   = "Input Right";
            port.symbol = "in_right";
            break;
        }
        port.groupId = kPortGroupStereo;
    }
    else
    {
        switch (index)
        {
        case 0:
            port.name    = "Output Left (Low)";
            port.symbol  = "in_left_low";
            port.groupId = kPortGroupLow;
            break;
        case 1:
            port.name    = "Output Right (Low)";
            port.symbol  = "in_right_low";
            port.groupId = kPortGroupLow;
            break;
        case 2:
            port.name    = "Output Left (Mid)";
            port.symbol  = "in_left_mid";
            port.groupId = kPortGroupMid;
            break;
        case 3:
            port.name    = "Output Right (Mid)";
            port.symbol  = "in_right_mid";
            port.groupId = kPortGroupMid;
            break;
        case 4:
            port.name    = "Output Left (High)";
            port.symbol  = "in_left_high";
            port.groupId = kPortGroupHigh;
            break;
        case 5:
            port.name    = "Output Right (High)";
            port.symbol  = "in_right_high";
            port.groupId = kPortGroupHigh;
            break;
        }
    }
}

void DistrhoPlugin3BandSplitter::initParameter(uint32_t index, Parameter& parameter)
{
    switch (index)
    {
    case paramLow:
        parameter.hints      = kParameterIsAutomatable;
        parameter.name       = "Low";
        parameter.symbol     = "low";
        parameter.unit       = "dB";
        parameter.ranges.def = 0.0f;
        parameter.ranges.min = -24.0f;
        parameter.ranges.max = 24.0f;
        break;

    case paramMid:
        parameter.hints      = kParameterIsAutomatable;
        parameter.name       = "Mid";
        parameter.symbol     = "mid";
        parameter.unit       = "dB";
        parameter.ranges.def = 0.0f;
        parameter.ranges.min = -24.0f;
        parameter.ranges.max = 24.0f;
        break;

    case paramHigh:
        parameter.hints      = kParameterIsAutomatable;
        parameter.name       = "High";
        parameter.symbol     = "high";
        parameter.unit       = "dB";
        parameter.ranges.def = 0.0f;
        parameter.ranges.min = -24.0f;
        parameter.ranges.max = 24.0f;
        break;

    case paramMaster:
        parameter.hints      = kParameterIsAutomatable;
        parameter.name       = "Master";
        parameter.symbol     = "master";
        parameter.unit       = "dB";
        parameter.ranges.def = 0.0f;
        parameter.ranges.min = -24.0f;
        parameter.ranges.max = 24.0f;
        break;

    case paramLowMidFreq:
        parameter.hints      = kParameterIsAutomatable;
        parameter.name       = "Low-Mid Freq";
        parameter.symbol     = "low_mid";
        parameter.unit       = "Hz";
        parameter.ranges.def = 440.0f;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 1000.0f;
        break;

    case paramMidHighFreq:
        parameter.hints      = kParameterIsAutomatable;
        parameter.name       = "Mid-High Freq";
        parameter.symbol     = "mid_high";
        parameter.unit       = "Hz";
        parameter.ranges.def = 1000.0f;
        parameter.ranges.min = 1000.0f;
        parameter.ranges.max = 20000.0f;
        break;
    }
}

void DistrhoPlugin3BandSplitter::initPortGroup(uint32_t groupId, PortGroup& portGroup)
{
    switch (groupId)
    {
    case kPortGroupLow:
        portGroup.name   = "Low";
        portGroup.symbol = "low";
        break;
    case kPortGroupMid:
        portGroup.name   = "Mid";
        portGroup.symbol = "mid";
        break;
    case kPortGroupHigh:
        portGroup.name   = "High";
        portGroup.symbol = "high";
        break;
    }
}

void DistrhoPlugin3BandSplitter::initProgramName(uint32_t index, String& programName)
{
    if (index != 0)
        return;

    programName = "Default";
}

// -----------------------------------------------------------------------
// Internal data

float DistrhoPlugin3BandSplitter::getParameterValue(uint32_t index) const
{
    switch (index)
    {
    case paramLow:
        return fLow;
    case paramMid:
        return fMid;
    case paramHigh:
        return fHigh;
    case paramMaster:
        return fMaster;
    case paramLowMidFreq:
        return fLowMidFreq;
    case paramMidHighFreq:
        return fMidHighFreq;
    default:
        return 0.0f;
    }
}

void DistrhoPlugin3BandSplitter::setParameterValue(uint32_t index, float value)
{
    if (getSampleRate() <= 0.0)
        return;

    switch (index)
    {
    case paramLow:
        fLow   = value;
        lowVol = std::exp( (fLow/48.0f) * 48.0f / kAMP_DB);
        break;
    case paramMid:
        fMid   = value;
        midVol = std::exp( (fMid/48.0f) * 48.0f / kAMP_DB);
        break;
    case paramHigh:
        fHigh   = value;
        highVol = std::exp( (fHigh/48.0f) * 48.0f / kAMP_DB);
        break;
    case paramMaster:
        fMaster = value;
        outVol  = std::exp( (fMaster/48.0f) * 48.0f / kAMP_DB);
        break;
    case paramLowMidFreq:
        fLowMidFreq = std::fmin(value, fMidHighFreq);
        freqLP = fLowMidFreq;
        xLP  = std::exp(-2.0f * kPI * freqLP / (float)getSampleRate());
        a0LP = 1.0f - xLP;
        b1LP = -xLP;
        break;
    case paramMidHighFreq:
        fMidHighFreq = std::fmax(value, fLowMidFreq);
        freqHP = fMidHighFreq;
        xHP  = std::exp(-2.0f * kPI * freqHP / (float)getSampleRate());
        a0HP = 1.0f - xHP;
        b1HP = -xHP;
        break;
    }
}

void DistrhoPlugin3BandSplitter::loadProgram(uint32_t index)
{
    if (index != 0)
        return;

    // Default values
    fLow = 0.0f;
    fMid = 0.0f;
    fHigh = 0.0f;
    fMaster = 0.0f;
    fLowMidFreq = 220.0f;
    fMidHighFreq = 2000.0f;

    // Internal stuff
    lowVol = midVol = highVol = outVol = 1.0f;
    freqLP = 200.0f;
    freqHP = 2000.0f;

    // reset filter values
    activate();
}

// -----------------------------------------------------------------------
// Process

void DistrhoPlugin3BandSplitter::activate()
{
    const float sr = (float)getSampleRate();

    xLP  = std::exp(-2.0f * kPI * freqLP / sr);

    a0LP = 1.0f - xLP;
    b1LP = -xLP;

    xHP  = std::exp(-2.0f * kPI * freqHP / sr);
    a0HP = 1.0f - xHP;
    b1HP = -xHP;
}

void DistrhoPlugin3BandSplitter::deactivate()
{
    out1LP = out2LP = out1HP = out2HP = 0.0f;
    tmp1LP = tmp2LP = tmp1HP = tmp2HP = 0.0f;
}

void DistrhoPlugin3BandSplitter::run(const float** inputs, float** outputs, uint32_t frames)
{
    const float* in1  = inputs[0];
    const float* in2  = inputs[1];
    float*       out1 = outputs[0];
    float*       out2 = outputs[1];
    float*       out3 = outputs[2];
    float*       out4 = outputs[3];
    float*       out5 = outputs[4];
    float*       out6 = outputs[5];

    for (uint32_t i=0; i < frames; ++i)
    {
        tmp1LP = a0LP * in1[i] - b1LP * tmp1LP + kDC_ADD;
        tmp2LP = a0LP * in2[i] - b1LP * tmp2LP + kDC_ADD;
        out1LP = tmp1LP - kDC_ADD;
        out2LP = tmp2LP - kDC_ADD;

        tmp1HP = a0HP * in1[i] - b1HP * tmp1HP + kDC_ADD;
        tmp2HP = a0HP * in2[i] - b1HP * tmp2HP + kDC_ADD;
        out1HP = in1[i] - tmp1HP - kDC_ADD;
        out2HP = in2[i] - tmp2HP - kDC_ADD;

        out6[i] = out2HP*highVol * outVol;
        out5[i] = out1HP*highVol * outVol;
        out4[i] = (in2[i] - out2LP - out2HP)*midVol * outVol;
        out3[i] = (in1[i] - out1LP - out1HP)*midVol * outVol;
        out2[i] = out2LP*lowVol * outVol;
        out1[i] = out1LP*lowVol * outVol;
    }
}

// -----------------------------------------------------------------------

Plugin* createPlugin()
{
    return new DistrhoPlugin3BandSplitter();
}

// -----------------------------------------------------------------------

END_NAMESPACE_DISTRHO
