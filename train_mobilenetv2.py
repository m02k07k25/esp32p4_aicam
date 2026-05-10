import tensorflow as tf
from tensorflow.keras.preprocessing.image import ImageDataGenerator
from tensorflow.keras.applications import MobileNetV2
from tensorflow.keras.layers import Dense, GlobalAveragePooling2D
from tensorflow.keras.models import Model
from tensorflow.keras.optimizers import Adam

# =========================
# 설정
# =========================
IMAGE_SIZE = 224
BATCH_SIZE = 4
EPOCHS = 10

DATASET_PATH = "dataset"

# =========================
# 데이터 전처리
# =========================
datagen = ImageDataGenerator(
    rescale=1./255,
    validation_split=0.2,
    rotation_range=15,
    zoom_range=0.1,
    horizontal_flip=True
)

train_data = datagen.flow_from_directory(
    DATASET_PATH,
    target_size=(IMAGE_SIZE, IMAGE_SIZE),
    batch_size=BATCH_SIZE,
    class_mode='binary',
    subset='training'
)

valid_data = datagen.flow_from_directory(
    DATASET_PATH,
    target_size=(IMAGE_SIZE, IMAGE_SIZE),
    batch_size=BATCH_SIZE,
    class_mode='binary',
    subset='validation'
)

# =========================
# MobileNetV2 모델 불러오기
# =========================
base_model = MobileNetV2(
    weights='imagenet',
    include_top=False,
    input_shape=(IMAGE_SIZE, IMAGE_SIZE, 3)
)

# 기존 학습된 가중치는 고정
base_model.trainable = False

# =========================
# 마지막 분류층 추가
# =========================
x = base_model.output
x = GlobalAveragePooling2D()(x)
x = Dense(128, activation='relu')(x)
output = Dense(1, activation='sigmoid')(x)

model = Model(inputs=base_model.input, outputs=output)

# =========================
# 모델 컴파일
# =========================
model.compile(
    optimizer=Adam(learning_rate=0.001),
    loss='binary_crossentropy',
    metrics=['accuracy']
)

# =========================
# 모델 학습
# =========================
history = model.fit(
    train_data,
    validation_data=valid_data,
    epochs=EPOCHS
)

# =========================
# 모델 저장
# =========================
model.save("mountain_person_mobilenetv2.h5")

# =========================
# TFLite 변환
# =========================
converter = tf.lite.TFLiteConverter.from_keras_model(model)
tflite_model = converter.convert()

with open("mountain_person_mobilenetv2.tflite", "wb") as f:
    f.write(tflite_model)

print("학습 완료")
print("TFLite 저장 완료")


# =========================
# INT8 Quantization
# =========================
converter = tf.lite.TFLiteConverter.from_keras_model(model)

converter.optimizations = [tf.lite.Optimize.DEFAULT]

quantized_tflite_model = converter.convert()

with open("mountain_person_mobilenetv2_int8.tflite", "wb") as f:
    f.write(quantized_tflite_model)

print("INT8 양자화 완료")