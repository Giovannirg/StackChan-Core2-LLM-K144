#pragma once
// Minimal AudioOutput stub — satisfies ESP8266SAM's #include <AudioOutput.h>
// without pulling in the full ESP8266Audio library (which brings I2S headers
// that conflict with M5Unified).

#include <stdint.h>

class AudioOutput {
public:
    virtual ~AudioOutput() {}
    virtual bool SetRate(int hz)         { hertz    = hz; return true; }
    virtual bool SetBitsPerSample(int b) { bps      = b;  return true; }
    virtual bool SetChannels(int c)      { channels = c;  return true; }
    virtual bool SetGain(float)          {                return true; }
    virtual bool begin()                 {                return true; }
    virtual bool ConsumeSample(int16_t sample[2]) = 0;
    virtual bool flush()                 {                return true; }
    virtual bool stop()                  {                return true; }
protected:
    int hertz    = 44100;
    int bps      = 16;
    int channels = 2;
};
