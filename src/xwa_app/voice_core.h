#ifndef XWA_APP_VOICE_CORE_H
#define XWA_APP_VOICE_CORE_H

/*
 * Voice commands, platform-free part: an English speech recognizer
 * (sherpa-onnx, loaded at run time; a non-streaming model that hears the
 * whole push-to-talk utterance) and the matcher from what was heard to a
 * flight command. The VR glue (microphone, push-to-talk, feedback) is in
 * vr_voice.c.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the recognizer library (`lib` as given to dlopen) and the model in
 * `model_dir` (encoder.onnx, decoder.onnx, joiner.onnx, tokens.txt: an
 * offline transducer). Slow (about a second): call it off the frame thread. Returns 1 when ready;
 * on failure `error` says why. */
int XwaVoice_Load(const char* lib, const char* model_dir, int threads, char* error, int error_size);
int XwaVoice_Ready(void);

/* Recognizes one utterance (16 kHz mono, -1..1) and writes what was said,
 * lower case. Takes a fraction of the utterance's length: call it off the
 * frame thread. */
void XwaVoice_Recognize(const float* samples, int count, char* out, int size);
void XwaVoice_Unload(void);

/* The command a heard phrase means. Returns 1 with the console action and
 * its argument, and a short label for the confirmation ("attack my
 * target"); 0 when nothing matches well enough. */
int XwaVoice_Match(const char* heard, int* action, int* arg, const char** label);

/* For the help text: the commands and one phrase each. */
int XwaVoice_CommandCount(void);
const char* XwaVoice_CommandPhrase(int index);

#ifdef __cplusplus
}
#endif

#endif
