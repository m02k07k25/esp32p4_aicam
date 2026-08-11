#pragma once

#define INFER_INPUT_WIDTH            (224)
#define INFER_INPUT_HEIGHT           (224)
#define INFER_LABEL_COUNT            (2)
#define INFER_HUMAN_CLASS_ID         (1)
#define INFER_CLASSIFY_JPEG_QUALITY  (60)
#define INFER_PREPROCESS_MODE_NAME   "resize"

#ifdef __cplusplus
extern "C" {
#endif

extern const char *g_infer_labels[INFER_LABEL_COUNT];

#ifdef __cplusplus
}
#endif
