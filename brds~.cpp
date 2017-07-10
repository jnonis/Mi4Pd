#include "m_pd.h"

//IMPROVE - inlets
//IMPROVE - variable  buffer size

#include <algorithm> 
#include "braids/macro_oscillator.h"
#include "braids/envelope.h"
#include "braids/vco_jitter_source.h"

// for signature_waveshaper, need abs
inline int16_t abs(int16_t x) { return x <0.0f ? -x : x;}
#include "braids/signature_waveshaper.h"

static t_class *brds_tilde_class;

inline float constrain(float v, float vMin, float vMax) {
  return std::max<float>(vMin,std::min<float>(vMax, v));
}

inline int constrain(int v, int vMin, int vMax) {
  return std::max<int>(vMin,std::min<int>(vMax, v));
}

typedef struct _brds_tilde {
  t_object  x_obj;

  braids::MacroOscillator osc;
  braids::Envelope envelope;
  braids::SignatureWaveshaper ws;
  braids::VcoJitterSource jitter_source;

  int16_t   previous_pitch;
  int16_t   previous_shape;
  uint16_t  gain_lp;
  bool      trigger_detected_flag;
  bool      trigger_flag;
  uint16_t  trigger_delay;


  t_float f_dummy;

  t_float f_shape;
  t_float f_pitch;
  t_float f_trig;
  t_float t_fm;
  t_float t_modulation;
  t_float f_colour;
  t_float f_timbre;

  t_float f_ad_attack;
  t_float f_ad_decay;
  t_float f_ad_mod_vca;
  t_float f_ad_mod_timbre;
  t_float f_ad_mod_colour;
  t_float f_ad_mod_pitch;
  t_float f_ad_mod_fm;

  t_float f_paques;
  t_float f_transposition;
  t_float f_auto_trig;
  t_float f_meta_modulation;
  t_float f_vco_drift;
  t_float f_vco_flatten;
  t_float f_fm_cv_offset;


  // CLASS_MAINSIGNALIN  = in_sync;
  t_inlet*  x_in_pitch;
  t_inlet*  x_in_shape;
  t_inlet*  x_in_trig;
  t_outlet* x_out;
} t_brds_tilde;


//define pure data methods
extern "C"  {
  t_int*  brds_tilde_render(t_int *w);
  void    brds_tilde_dsp(t_brds_tilde *x, t_signal **sp);
  void    brds_tilde_free(t_brds_tilde *x);
  void*   brds_tilde_new(t_floatarg f);
  void    brds_tilde_setup(void);

  void brds_tilde_pitch(t_brds_tilde *x, t_floatarg f);
  void brds_tilde_shape(t_brds_tilde *x, t_floatarg f);
  void brds_tilde_colour(t_brds_tilde *x, t_floatarg f);
  void brds_tilde_timbre(t_brds_tilde *x, t_floatarg f);
}





// puredata methods implementation -start
t_int* brds_tilde_perform(t_int *w)
{
  t_brds_tilde *x = (t_brds_tilde *)(w[1]);
  t_sample  *in_sync  =    (t_sample *)(w[2]);
  t_sample  *out =    (t_sample *)(w[3]);
  int n =  (int)(w[4]);

  x->envelope.Update(int(x->f_ad_attack * 8.0f ) , int(x->f_ad_decay * 8.0f) );
  uint32_t ad_value = x->envelope.Render();


  if (x->f_paques) {
    x->osc.set_shape(braids::MACRO_OSC_SHAPE_QUESTION_MARK);
  } else if (x->f_meta_modulation) {
    int16_t shape = (braids::MacroOscillatorShape) (x->f_shape * braids::MACRO_OSC_SHAPE_LAST);
    shape -= x->f_fm_cv_offset;
    if (shape > x->previous_shape + 2 || shape < x->previous_shape - 2) {
      x->previous_shape = shape;
    } else {
      shape = x->previous_shape;
    }
    shape = braids::MACRO_OSC_SHAPE_LAST * shape >> 11;
    shape += x->f_shape;
    if (shape >= braids::MACRO_OSC_SHAPE_LAST_ACCESSIBLE_FROM_META) {
      shape = braids::MACRO_OSC_SHAPE_LAST_ACCESSIBLE_FROM_META;
    } else if (shape <= 0) {
      shape = 0;
    }
    braids::MacroOscillatorShape osc_shape = static_cast<braids::MacroOscillatorShape>(shape);
    x->osc.set_shape(osc_shape);
  } else {
    x->osc.set_shape((braids::MacroOscillatorShape) (x->f_shape * braids::MACRO_OSC_SHAPE_LAST));
  }
  
  int32_t timbre = int(x->f_timbre * 32768.0f);
  timbre += ad_value * x->f_ad_mod_timbre;
  int32_t colour = int(x->f_colour * 32768.0f);
  colour += ad_value * x->f_ad_mod_colour;
  x->osc.set_parameters(constrain(timbre,0,32768), constrain(colour,0,32768));


  int32_t pitch = x->f_pitch;
  // if (!settings.meta_modulation()) {
  //   pitch += settings.adc_to_fm(adc.channel(3));
  // }

  // Check if the pitch has changed to cause an auto-retrigger
  int32_t pitch_delta = pitch - x->previous_pitch;
  if (x->f_auto_trig &&
      (pitch_delta >= 0x40 || -pitch_delta >= 0x40)) {
    x->trigger_detected_flag = true;
  }
  x->previous_pitch = pitch; 
  
  // pitch += jitter_source.Render(x->f_vco_drift);
  pitch += ad_value * x->f_ad_mod_pitch;
  
  if (pitch > 16383) {
    pitch = 16383;
  } else if (pitch < 0) {
    pitch = 0;
  }
  
//   if (x->f_vco_flatten) {
//     pitch = Interpolate88(braids::lut_vco_detune, pitch << 2);
//   }
  x->osc.set_pitch(pitch + x->f_transposition);

  if (x->trigger_flag) {
    x->osc.Strike();
    x->envelope.Trigger(braids::ENV_SEGMENT_ATTACK);
    x->trigger_flag = false;
  }

  bool sync_zero = x->f_ad_mod_vca!=0  || x->f_ad_mod_timbre !=0 || x->f_ad_mod_colour !=0 || x->f_ad_mod_fm !=0; 

  uint8_t* sync = new uint8_t[n];
  int16_t* outint = new int16_t[n];

  for (int i = 0; i < n; i++) {
    if(sync_zero) sync[i] = 0;
    else sync[i] = in_sync[i] * (1<<8) ;
  }

  x->osc.Render(sync, outint, n);

  for (int i = 0; i < n; i++) {
    out[i] = outint[i] / 65536.0f ;
  }

//   // Copy to DAC buffer with sample rate and bit reduction applied.
//   int16_t sample = 0;
//   size_t decimation_factor = decimation_factors[settings.data().sample_rate];
//   uint16_t bit_mask = bit_reduction_masks[settings.data().resolution];
//   int32_t gain = settings.GetValue(SETTING_AD_VCA) ? ad_value : 65535;
//   uint16_t signature = settings.signature() * settings.signature() * 4095;
//   for (size_t i = 0; i < kBlockSize; ++i) {
//     if ((i % decimation_factor) == 0) {
//       sample = render_buffer[i] & bit_mask;
//     }
//     sample = sample * gain_lp >> 16;
//     gain_lp += (gain - gain_lp) >> 4;
//     int16_t warped = ws.Transform(sample);
//     render_buffer[i] = Mix(sample, warped, signature);
//   }

  // bool trigger_detected = gate_input.raised();
  // sync_samples[playback_block][current_sample] = trigger_detected;
  // trigger_detected_flag = trigger_detected_flag | trigger_detected;
  
  // if (trigger_detected_flag) {
  //   trigger_delay = settings.trig_delay()
  //       ? (1 << settings.trig_delay()) : 0;
  //   ++trigger_delay;
  //   trigger_detected_flag = false;
  // }
  // if (trigger_delay) {
  //   --trigger_delay;
  //   if (trigger_delay == 0) {
  //     trigger_flag = true;
  //   }
  // }


  delete [] sync;
  delete [] outint;

  return (w + 5); // # args + 1
}

void brds_tilde_dsp(t_brds_tilde *x, t_signal **sp)
{
  // add the perform method, with all signal i/o
  dsp_add(brds_tilde_render, 4,
          x,
          sp[0]->s_vec, sp[1]->s_vec, // signal i/o (clockwise)
          sp[0]->s_n);
}

void brds_tilde_free(t_brds_tilde *x)
{
  inlet_free(x->x_in_shape);
  inlet_free(x->x_in_pitch);
  outlet_free(x->x_out);
}

void *brds_tilde_new(t_floatarg f)
{
  t_brds_tilde *x = (t_brds_tilde *) pd_new(brds_tilde_class);

  x->previous_pitch = 0;
  x->previous_shape = 0;
  x->gain_lp = 0;
  x->trigger_detected_flag = false;
  x->trigger_flag = false;
  x->trigger_delay = 0;


  x->f_dummy = f;
  x->f_shape = 0.0f;
  x->f_pitch = 0.0f;
  x->f_trig = 0.0f;
  x->t_fm = 0.0f;
  x->t_modulation = 0.0f;
  x->f_colour = 0.0f;
  x->f_timbre = 0.0f;
  x->f_ad_attack = 0.0f;
  x->f_ad_decay = 0.0f;
  x->f_ad_mod_vca = 0.0f;
  x->f_ad_mod_timbre = 0.0f;
  x->f_ad_mod_colour = 0.0f;
  x->f_ad_mod_pitch = 0.0f;
  x->f_ad_mod_fm = 0.0f;
  x->f_paques = 0.0f;
  x->f_transposition = 0.0f;
  x->f_auto_trig = 0.0f;
  x->f_meta_modulation = 0.0f;
  x->f_vco_drift = 0.0f;
  x->f_vco_flatten = 0.0f;
  x->f_fm_cv_offset = 0.0f;

  x->x_in_shape  = floatinlet_new (&x->x_obj, &x->f_shape);
  x->x_in_pitch  = floatinlet_new (&x->x_obj, &x->f_pitch);
  x->x_out   = outlet_new(&x->x_obj, &s_signal);

  x->osc.Init();
  return (void *)x;
}

void brds_tilde_pitch(t_brds_tilde *x, t_floatarg f)
{
  x->f_pitch = f;
}
void brds_tilde_shape(t_brds_tilde *x, t_floatarg f)
{
  x->f_shape = f;
}
void brds_tilde_colour(t_brds_tilde *x, t_floatarg f)
{
  x->f_colour = f;
}
void brds_tilde_timbre(t_brds_tilde *x, t_floatarg f)
{
  x->f_timbre = f;
}

void brds_tilde_setup(void) {
  brds_tilde_class = class_new(gensym("brds~"),
                                         (t_newmethod)brds_tilde_new,
                                         0, sizeof(t_brds_tilde),
                                         CLASS_DEFAULT,
                                         A_DEFFLOAT, A_NULL);

  class_addmethod(  brds_tilde_class,
                    (t_method)brds_tilde_dsp,
                    gensym("dsp"), A_NULL);
  CLASS_MAINSIGNALIN(brds_tilde_class, t_brds_tilde, f_dummy);


  class_addmethod(brds_tilde_class,
                  (t_method) brds_tilde_pitch, gensym("pitch"),
                  A_DEFFLOAT, A_NULL);
  class_addmethod(brds_tilde_class,
                  (t_method) brds_tilde_shape, gensym("shape"),
                  A_DEFFLOAT, A_NULL);
  class_addmethod(brds_tilde_class,
                  (t_method) brds_tilde_colour, gensym("colour"),
                  A_DEFFLOAT, A_NULL);
  class_addmethod(brds_tilde_class,
                  (t_method) brds_tilde_timbre, gensym("timbre"),
                  A_DEFFLOAT, A_NULL);
}
// puredata methods implementation - end
