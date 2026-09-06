/*
 * model_data.h - placeholder for an exported TFLite Micro model.
 *
 * Replace with the output of `xxd -i your_model.tflite > model_data.h`
 * (defines an array + `_len` constant), rename to match below, and use in
 * model_runner.c/.cpp when constructing the TFLM interpreter.
 */
#ifndef _MODEL_DATA_H_
#define _MODEL_DATA_H_

/* No model linked yet - see model_runner.c for integration instructions. */
static const unsigned char g_model_data[] = {0x00};
static const unsigned int g_model_data_len = 0;

#endif /* _MODEL_DATA_H_ */
