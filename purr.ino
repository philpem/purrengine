/**
 * "The Purrmachine"
 * Cat purr simulator
 * Phil Pemberton <philpem@philpem.me.uk>
 * 
 * Libraries required:
 *   Libfilter -- https://github.com/MartinBloedorn/libFilter  (requires EPS renaming to EPSILON due to conflict with ESP32 libs)
 *   ESP8266Audio -- https://github.com/earlephilhower/ESP8266Audio
 *   ESP8266_Spiram (required by ESP8266Audio) -- https://github.com/Gianbacchio/ESP8266_Spiram
 * 
 * TODO --
 *   Input from switches (GPIOs) to turn purring on/off.
 *    See https://learn.sparkfun.com/tutorials/i2s-audio-breakout-hookup-guide/all
 *   Move purr settings into a struct (to allow profile switching)
 *   Purr state into a struct too, allow resetting (e.g. on start/stop)
 * 
 * TODO - document parameters
 *   - Purr rate ({inh,exh}IMP_FREQ)
 *   - Purr delay (delay between inh/exh cycles -- 1-2 seconds) --- implemented, "rest time"
 *   - Inh/exh delay --- implemented, "breath hold"
 *   - Inh/exh Symmetry (inhale/exhale ratio) -- done, exhale_factor
 *   - Inh/exh Frequency shift -- calculated from Symmetry (a few Hz at 50%) -- TODO!!!
 *   - Vocal tract cutoff frequency (VT_FREQ)
 *   - Impulse inverse/forward pulse widths
 * TODO - features to add
 *   - Inhale/exhale amplitude envelope (ADSR envelope -- https://www.wikiaudio.org/adsr-envelope/)
 */

/*

Purr impulse structure -- exhale:

  +---+            +-
  |   |            |
--+   +   +--------+   ...
      |   |
      +---+

  |fwd|rev|
  |----- cycle ----|

Inhale cycles are the same but with the fwd/rev polarities swapped.


Breath cycle:

  iiiIIIIIIIiii____eeeEEEEEEEeee________ii
  |--inhale---|hold|---exhale--|--rest--|

Inhale and Exhale are made up of purr impulses.
Lower case sections are the attack/decay ramps (TODO)

 */


#include <Arduino.h>
#include <AudioOutput.h>
#include <AudioOutputI2S.h>
#include <filters.h>


//// I/O pins

// Switches
#define IO_SWITCH1  15

// Sound channels
#define AUDIO_MUZZLE  AudioOutput::LEFTCHANNEL
#define AUDIO_CHEST   AudioOutput::RIGHTCHANNEL


//// Purr configuration

const int SAMPLE_RATE = 44100;
const float sampling_time = 1.0/SAMPLE_RATE;


// Set when settings have changed
bool PurrConfigUpdate = true;

//
// Impulse consists of an negative pulse, a positive pulse and a rest time.
// (exhale is the opposite -- a positive followed by a negative)
//

// Impulse amplitude
float inh_ampl = 0.2;               // inhale
float exh_ampl = inh_ampl * 0.5;    // exhale

// Inhale impulse parameters
float inh_imp_freq = 30.0;
float inh_imp_invwid = 0.2;
float inh_imp_fwdwid = 0.3;

// Exhale impulse parameters
float exh_imp_freq = 28.0;
float exh_imp_invwid = 0.2;
float exh_imp_fwdwid = 0.3;

// Vocal tract parameters
float vocal_cutoff_freq = 180.0;

// Purr rate (seconds per breath in+out)
float inhale_time = 1.2;
float exhale_factor = 1.3;    // inhale/exhale symmetry (exhale time is this * inhale_time)
float breath_hold = 0.1;      // silence between inhale and exhale
float breath_rest = 0.5;      // rest time between breaths  -- increasing this makes the purr more "relaxed"


// Audio objects
AudioOutputI2S *output;
Filter *vocalFilter;

// use rounding when converting from float to signed
#define ROUNDCONV

// Saturating float to signed16
int16_t floatToSigned(const float samp)
{
  if (samp > 1.0) {
    return 32767;
  } else if (samp < -1.0) {
    return -32767;
  } else {
#ifdef ROUNDCONV
    return roundf(samp * 32767);
#else
    return samp * 32767;
#endif
  }
}


volatile bool soundactive_toggle = false;

// External Interrupt function with software switch debounce
void IRAM_ATTR handleInterrupt()
{
  static unsigned long last_interrupt_time = 0;
  unsigned long interrupt_time = millis();
  
  // If interrupts come faster than 200ms, assume it's a bounce and ignore
  if (interrupt_time - last_interrupt_time > 200) 
  {
    soundactive_toggle = true;
  }
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  delay(1000);

  // Set I/O port mode
  pinMode(IO_SWITCH1, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(IO_SWITCH1), handleInterrupt, FALLING);
  
  //SPIFFS.begin();
  //file = new AudioFileSourceSPIFFS("/jamonit.mp3");
  //out = new AudioOutputI2SNoDAC();
  //mp3 = new AudioGeneratorMP3();
  //mp3->begin(file, out);

  printf("creating filter...\n");
  // Initialise vocal tract filter
  vocalFilter = new Filter(vocal_cutoff_freq, sampling_time, IIR::ORDER::OD3);

  printf("setting up audio...\n");
  // Set up audio output on I2S port using internal DAC
  output = new AudioOutputI2S(0, AudioOutputI2S::INTERNAL_DAC);
  output->SetRate(SAMPLE_RATE);
  output->SetChannels(2);
  output->SetBitsPerSample(16);
  output->begin();

  printf("launch!\n");
}




void loop() {
  // put your main code here, to run repeatedly:

/*
  if (mp3->isRunning()) {
    if (!mp3->loop()) mp3->stop(); 
  } else {
    Serial.printf("MP3 done\n");
    delay(1000);
  }
*/


  // impulse generator thresholds
  static unsigned int inh_impulse_width_samps, exh_impulse_width_samps;  // width of an impulse cycle
  static unsigned int inh_impulse_fwdtime, exh_impulse_fwdtime;
  static unsigned int inh_impulse_revtime, exh_impulse_revtime;


  // cycle timings
  int inh_impulse_n, hold_n, exh_impulse_n, rest_n;


  // impulse generator state
  static unsigned int imp_time_samps = 0;
  static unsigned int cyc_time_samps = 0;

  static int yields = 0;
  static bool first = true;

  static bool soundactive_latched = false;

  if ((cyc_time_samps == 0) && !first) {
    printf("purr cycle complete, yields: %d (more is better)\n", yields);
    printf("   switch: %d\n", soundactive_toggle);
    if (soundactive_toggle) {
      soundactive_latched = !soundactive_latched;
      soundactive_toggle = false;
    }
    yields = 0;
  }
  if (first) {
    first = false;
  }


  // Update local config cache if settings have changed
  if (PurrConfigUpdate) {
    //vocalFilter.setcutoff(vocal_cutoff_freq);

    // Calculate number of samples per purr-impulse cycle
    inh_impulse_width_samps = SAMPLE_RATE / inh_imp_freq;
    exh_impulse_width_samps = SAMPLE_RATE / exh_imp_freq;


    // Calculate inh/exh fwd/rev times
    inh_impulse_revtime = inh_impulse_width_samps * inh_imp_invwid;
    inh_impulse_fwdtime = inh_impulse_revtime + (inh_impulse_width_samps * inh_imp_fwdwid);

    exh_impulse_revtime = exh_impulse_width_samps * exh_imp_invwid;
    exh_impulse_fwdtime = exh_impulse_revtime + (exh_impulse_width_samps * exh_imp_fwdwid);


    // Calculate sample points where state changes happen
    inh_impulse_n =                 (SAMPLE_RATE * inhale_time);
    hold_n        = inh_impulse_n + (SAMPLE_RATE * breath_hold);
    exh_impulse_n = hold_n        + (SAMPLE_RATE * inhale_time * exhale_factor);
    rest_n        = exh_impulse_n + (SAMPLE_RATE * breath_rest);
  }


  // ---- Impulse generator ---- //

  float imp_samp;   // Impulse output sample

  float progress;   // 0.0 to 1.0 depending on how far we are thru the 

  if (cyc_time_samps < inh_impulse_n) {
    // -- Inhaling --
    // if time in samples is more than the inhale impulse width, reset
    if (imp_time_samps >= inh_impulse_width_samps) {
      imp_time_samps = 0;
    }
   
    if (imp_time_samps == 0) {
      imp_samp = 0;
    } else if (imp_time_samps < inh_impulse_revtime) {
      imp_samp = -inh_ampl;
    } else if (imp_time_samps < inh_impulse_fwdtime) {
      imp_samp = inh_ampl;
    } else {
      imp_samp = 0.0;
    }

    // Calculate % completion of inhale phase
    progress = (float)cyc_time_samps / (float)inh_impulse_n;
    
  } else if (cyc_time_samps < hold_n) {
    // -- Holding --
    imp_samp = 0.0;

  } else if (cyc_time_samps < exh_impulse_n) {
    // -- Exhaling --
    // if time in samples is more than the exhale impulse width, reset
    if (imp_time_samps >= exh_impulse_width_samps) {
      imp_time_samps = 0;
    }

    if (imp_time_samps == 0) {
      imp_samp = 0;
    } else if (imp_time_samps < exh_impulse_revtime) {
      imp_samp = exh_ampl;
    } else if (imp_time_samps < exh_impulse_fwdtime) {
      imp_samp = -exh_ampl;
    } else {
      imp_samp = 0.0;
    }

    // Calculate % completion of exhale phase
    progress = (float)(cyc_time_samps - hold_n) / (float)(exh_impulse_n - hold_n);
    
  } else {
    // -- Rest time between breaths --
    imp_samp = 0.0;
  }

  // we're done, move to next impulse time
  imp_time_samps++;

  // move to next cycle time too
  cyc_time_samps++;
  if (cyc_time_samps >= rest_n) {
    cyc_time_samps = 0;
  }


  // At this point imp_samp is an impulse sample


  // ---- Sinusoidal envelope ---- //
  // (because I'm too lazy to implement ADSR)

  float envelope = sinf(progress * PI);
  imp_samp *= envelope;


  // ---- Vocal tract filter ---- //

  float filtered_impulse = vocalFilter->filterIn(imp_samp);
 

  // Convert the sample to signed16 then pass it to the I2S and DAC
  int16_t samp[2];

  if (soundactive_latched) {
    samp[AUDIO_MUZZLE] = floatToSigned(filtered_impulse);
    samp[AUDIO_CHEST]  = floatToSigned(filtered_impulse);
  } else {
    samp[AudioOutput::LEFTCHANNEL] = 0;
    samp[AudioOutput::RIGHTCHANNEL] = 0;
  }

  while (!output->ConsumeSample(samp)) { yields++; yield(); };

// Can do output->ConsumeSample to send one sample
// Can do output->ConsumeSamples to send a buffer

}
