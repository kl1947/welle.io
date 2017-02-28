/*
 *    Copyright (C) 2017
 *    Albrecht Lohofener (albrechtloh@gmx.de)
 *
 *    This file is based on SDR-J
 *    Copyright 2015 by Andrea Montefusco IW0HDV
 *    Copyright (C) 2010, 2011, 2012, 2013
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *
 *    This file is part of the welle.io.
 *    Many of the ideas as implemented in welle.io are derived from
 *    other work, made available through the GNU general Public License.
 *    All copyrights of the original authors are recognized.
 *
 *    welle.io is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    welle.io is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License 3.0+
 *    along with welle.io; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
*/

#include "CAirspy.h"

static const int EXTIO_NS = 8192;
static const int EXTIO_BASE_TYPE_SIZE = sizeof(float);

CAirspy::CAirspy(QSettings* s, bool* success)
{
    int result;
    int distance = 10000000;
    uint32_t myBuffer[20];
    uint32_t samplerate_count;

    this->airspySettings = s;

    *success = false;
    airspySettings->beginGroup("airspy");
    vgaGain = airspySettings->value("vga", 5).toInt();
    mixerGain = airspySettings->value("mixer", 10).toInt();
    lnaGain = airspySettings->value("lna", 5).toInt();
    airspySettings->endGroup();

    mixer_agc = false;
    lna_agc = false;
    rf_bias = false;

    device = 0;
    serialNumber = 0;
    SampleBuffer = NULL;
    SpectrumSampleBuffer = NULL;

    libraryLoaded = true;

    strcpy(serial, "");
    result = airspy_init();
    if (result != AIRSPY_SUCCESS) {
        printf("airspy_init () failed: %s (%d)\n",
            airspy_error_name((airspy_error)result), result);
        return;
    }

    fprintf(stderr, "airspy init is succesfully\n");
    result = airspy_open(&device);
    if (result != AIRSPY_SUCCESS) {
        printf("my_airpsy_open () failed: %s (%d)\n",
            airspy_error_name((airspy_error)result), result);
        return;
    }

    airspy_set_sample_type(device, AIRSPY_SAMPLE_INT16_IQ);
    airspy_get_samplerates(device, &samplerate_count, 0);
    fprintf(stderr, "%d samplerates are supported\n", samplerate_count);
    airspy_get_samplerates(device, myBuffer, samplerate_count);

    selectedRate = 0;
    for (uint32_t i = 0; i < samplerate_count; i++) {
        fprintf(stderr, "%d \n", myBuffer[i]);
        if (abs(myBuffer[i] - 2048000) < distance) {
            distance = abs(myBuffer[i] - 2048000);
            selectedRate = myBuffer[i];
        }
    }

    if (selectedRate == 0) {
        fprintf(stderr, "Sorry. cannot help you\n");
        return;
    } else
        fprintf(stderr, "selected samplerate = %d\n", selectedRate);

    result = airspy_set_samplerate(device, selectedRate);
    if (result != AIRSPY_SUCCESS) {
        printf("airspy_set_samplerate() failed: %s (%d)\n",
            airspy_error_name((enum airspy_error)result), result);
        return;
    }

    //	The sizes of the mapTable and the convTable are
    //	predefined and follow from the input and output rate
    //	(selectedRate / 1000) vs (2048000 / 1000)
    convBufferSize = selectedRate / 1000;
    for (uint32_t i = 0; i < 2048; i++) {
        float inVal = float(selectedRate / 1000);
        mapTable_int[i] = int(floor(i * (inVal / 2048.0)));
        mapTable_float[i] = i * (inVal / 2048.0) - mapTable_int[i];
    }
    convIndex = 0;
    convBuffer = new DSPCOMPLEX[convBufferSize + 1];

    SampleBuffer = new RingBuffer<DSPCOMPLEX>(256 * 1024);
    SpectrumSampleBuffer = new RingBuffer<DSPCOMPLEX>(256 * 1024);

    result = airspy_set_lna_agc(device, 1);
    if (result != AIRSPY_SUCCESS)
        printf("airspy_set_lna_agc() failed: %s (%d)\n", airspy_error_name((airspy_error)result), result);

    result = airspy_set_mixer_agc(device, 1);
    if (result != AIRSPY_SUCCESS)
        printf("airspy_set_mixer_agc() failed: %s (%d)\n", airspy_error_name((airspy_error)result), result);

    result = airspy_set_vga_gain(device, 15);
    if (result != AIRSPY_SUCCESS)
       printf("airspy_set_vga_gain () failed: %s (%d)\n", airspy_error_name((airspy_error)result), result);

    running = false;

    *success = true;
    return;
}

CAirspy::~CAirspy(void)
{
    airspySettings->beginGroup("airspy");
    airspySettings->setValue("vga", vgaGain);
    airspySettings->setValue("mixer", mixerGain);
    airspySettings->setValue("lna", lnaGain);
    airspySettings->endGroup();

    if (device) {
        int result = airspy_stop_rx(device);
        if (result != AIRSPY_SUCCESS) {
            printf("airspy_stop_rx () failed: %s (%d)\n",
                airspy_error_name((airspy_error)result), result);
        }

        result = airspy_close(device);
        if (result != AIRSPY_SUCCESS) {
            printf("airspy_close () failed: %s (%d)\n",
                airspy_error_name((airspy_error)result), result);
        }
    }
    airspy_exit();
}

void CAirspy::setFrequency(int32_t nf)
{
    int result = airspy_set_freq(device, nf);

    if (result != AIRSPY_SUCCESS) {
        printf("airspy_set_freq() failed: %s (%d)\n",
            airspy_error_name((airspy_error)result), result);
    }
}

bool CAirspy::restart(void)
{
    int result;
    int32_t bufSize = EXTIO_NS * EXTIO_BASE_TYPE_SIZE * 2;
    if (running)
        return true;

    SampleBuffer->FlushRingBuffer();
    SpectrumSampleBuffer->FlushRingBuffer();
    result = airspy_set_sample_type(device, AIRSPY_SAMPLE_INT16_IQ);
    if (result != AIRSPY_SUCCESS) {
        printf("airspy_set_sample_type () failed: %s (%d)\n",
            airspy_error_name((airspy_error)result), result);
        return false;
    }

    result = airspy_start_rx(device,
        (airspy_sample_block_cb_fn)callback, this);
    if (result != AIRSPY_SUCCESS) {
        printf("airspy_start_rx () failed: %s (%d)\n",
            airspy_error_name((airspy_error)result), result);
        return false;
    }

    buffer = new uint8_t[bufSize];
    bs_ = bufSize;
    bl_ = 0;
    running = true;
    return true;
}

void CAirspy::stop(void)
{
    if (!running)
        return;
    int result = airspy_stop_rx(device);

    if (result != AIRSPY_SUCCESS) {
        printf("airspy_stop_rx() failed: %s (%d)\n",
            airspy_error_name((airspy_error)result), result);
    } else {
        delete[] buffer;
        bs_ = bl_ = 0;
    }
    running = false;
}

int CAirspy::callback(airspy_transfer* transfer)
{
    CAirspy* p;

    if (!transfer)
        return 0; // should not happen
    p = static_cast<CAirspy*>(transfer->ctx);

    // AIRSPY_SAMPLE_FLOAT32_IQ:
    uint32_t bytes_to_write = transfer->sample_count * sizeof(int16_t) * 2;
    uint8_t* pt_rx_buffer = (uint8_t*)transfer->samples;

    while (bytes_to_write > 0) {
        int spaceleft = p->bs_ - p->bl_;
        int to_copy = std::min((int)spaceleft, (int)bytes_to_write);
        ::memcpy(p->buffer + p->bl_, pt_rx_buffer, to_copy);
        bytes_to_write -= to_copy;
        pt_rx_buffer += to_copy;
        //
        //	   bs (i.e. buffersize) in bytes
        if (p->bl_ == p->bs_) {
            p->data_available((void*)p->buffer, p->bl_);
            p->bl_ = 0;
        }
        p->bl_ += to_copy;
    }
    return 0;
}

//	called from AIRSPY data callback
//	this method is declared in airspyHandler class
//	The buffer received from hardware contains
//	32-bit floating point IQ samples (8 bytes per sample)
//
//	recoded for the sdr-j framework
//	2*2 = 4 bytes for sample, as per AirSpy USB data stream format
//	we do the rate conversion here, read in groups of 2 * 625 samples
//	and transform them into groups of 2 * 512 samples
int CAirspy::data_available(void* buf, int buf_size)
{
    int16_t* sbuf = (int16_t*)buf;
    int nSamples = buf_size / (sizeof(int16_t) * 2);
    DSPCOMPLEX temp[2048];
    int32_t i, j;

    for (i = 0; i < nSamples; i++) {
        convBuffer[convIndex++] = DSPCOMPLEX(sbuf[2 * i] / (float)2048,
            sbuf[2 * i + 1] / (float)2048);
        if (convIndex > convBufferSize) {
            for (j = 0; j < 2048; j++) {
                int16_t inpBase = mapTable_int[j];
                float inpRatio = mapTable_float[j];
                temp[j] = cmul(convBuffer[inpBase + 1], inpRatio) + cmul(convBuffer[inpBase], 1 - inpRatio);
            }

            SampleBuffer->putDataIntoBuffer(temp, 2048);
            SpectrumSampleBuffer->putDataIntoBuffer(temp, 2048);
            //
            //	shift the sample at the end to the beginning, it is needed
            //	as the starting sample for the next time
            convBuffer[0] = convBuffer[convBufferSize];
            convIndex = 1;
        }
    }
    return 0;
}

void CAirspy::reset(void)
{
    SampleBuffer->FlushRingBuffer();
    SpectrumSampleBuffer->FlushRingBuffer();
}

int32_t CAirspy::getSamples(DSPCOMPLEX* Buffer, int32_t Size)
{

    return SampleBuffer->getDataFromBuffer(Buffer, Size);
}

int32_t CAirspy::getSpectrumSamples(DSPCOMPLEX *Buffer, int32_t Size)
{
    return SpectrumSampleBuffer->getDataFromBuffer(Buffer, Size);
}

int32_t CAirspy::getSamplesToRead(void)
{
    return SampleBuffer->GetRingBufferReadAvailable();
}

void CAirspy::setAgc(bool AGC)
{
    (void) AGC;
}

void CAirspy::setGain(int value)
{
    (void) value;
}
