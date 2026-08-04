function(validate_classifier_model classifier_model)
    set(classifier_manifest "${classifier_model}.json")
    if (NOT EXISTS "${classifier_model}")
        message(FATAL_ERROR
            "Missing classifier model: ${classifier_model}\n"
            "Generate model/artifacts/espdl/classifier_224_p4.espdl with model/quantize_espdl.py before building.")
    endif()
    if (NOT EXISTS "${classifier_manifest}")
        message(FATAL_ERROR
            "Missing classifier compatibility manifest: ${classifier_manifest}\n"
            "The existing .espdl may use the old width/preprocessing. Regenerate it with the current model/quantize_espdl.py.")
    endif()

    file(READ "${classifier_manifest}" classifier_manifest_json)
    string(JSON classifier_architecture ERROR_VARIABLE classifier_manifest_error
           GET "${classifier_manifest_json}" architecture)
    if (NOT classifier_manifest_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "Invalid classifier compatibility manifest: ${classifier_manifest_error}\n"
            "Regenerate ${classifier_model} with the current model/quantize_espdl.py.")
    endif()

    string(JSON classifier_format_version GET "${classifier_manifest_json}" format_version)
    string(JSON classifier_width_mult GET "${classifier_manifest_json}" width_mult)
    string(JSON classifier_image_size GET "${classifier_manifest_json}" image_size)
    string(JSON classifier_preprocess GET "${classifier_manifest_json}" preprocess_profile)
    string(JSON classifier_label_count LENGTH "${classifier_manifest_json}" labels)
    string(JSON classifier_label_0 GET "${classifier_manifest_json}" labels 0)
    string(JSON classifier_label_1 GET "${classifier_manifest_json}" labels 1)
    string(JSON classifier_aggregation GET "${classifier_manifest_json}" inference aggregation)
    string(JSON classifier_human_class GET "${classifier_manifest_json}" inference human_class)
    string(JSON classifier_human_threshold GET "${classifier_manifest_json}" inference human_threshold)
    string(JSON classifier_decision_rule GET "${classifier_manifest_json}" inference decision_rule)
    string(JSON classifier_threshold_source GET "${classifier_manifest_json}" inference threshold_source)
    string(JSON classifier_target GET "${classifier_manifest_json}" quantization target)
    string(JSON classifier_bits GET "${classifier_manifest_json}" quantization bits)
    string(JSON classifier_precision_profile GET "${classifier_manifest_json}" quantization profile)
    string(JSON classifier_calibration_algorithm GET "${classifier_manifest_json}" quantization calibration_algorithm)
    string(JSON classifier_mixed_count LENGTH "${classifier_manifest_json}" quantization mixed_int16_operations)
    string(JSON classifier_mixed_0 GET "${classifier_manifest_json}" quantization mixed_int16_operations 0)
    string(JSON classifier_mixed_1 GET "${classifier_manifest_json}" quantization mixed_int16_operations 1)
    string(JSON classifier_expected_sha256 GET "${classifier_manifest_json}" espdl_sha256)

    if (NOT classifier_format_version STREQUAL "2"
        OR NOT classifier_architecture STREQUAL "mobilenet_v2_035_keras_tf_same"
        OR NOT classifier_width_mult STREQUAL "0.35"
        OR NOT classifier_image_size STREQUAL "224"
        OR NOT classifier_preprocess STREQUAL "keras_mobilenet_v2_minus_one_to_one"
        OR NOT classifier_label_count STREQUAL "2"
        OR NOT classifier_label_0 STREQUAL "no_human"
        OR NOT classifier_label_1 STREQUAL "human"
        OR NOT classifier_aggregation STREQUAL "max_human_score_over_five_half_frame_crops"
        OR NOT classifier_human_class STREQUAL "human"
        OR NOT classifier_human_threshold STREQUAL "0.72482645511627197"
        OR NOT classifier_decision_rule STREQUAL "gte"
        OR NOT classifier_threshold_source STREQUAL "validation_balanced_accuracy"
        OR NOT classifier_target STREQUAL "esp32p4"
        OR NOT classifier_bits STREQUAL "8"
        OR NOT classifier_precision_profile STREQUAL "mixed_int8_int16"
        OR NOT classifier_calibration_algorithm STREQUAL "kl"
        OR NOT classifier_mixed_count STREQUAL "2"
        OR NOT classifier_mixed_0 STREQUAL "/features/features.1/conv/conv.0/conv.0.0/Conv"
        OR NOT classifier_mixed_1 STREQUAL "/features/features.1/conv/conv.0/conv.0.2/Clip")
        message(FATAL_ERROR
            "Incompatible classifier metadata in ${classifier_manifest}.\n"
            "Expected MobileNetV2 0.35, 224x224, Keras [-1,1] preprocessing, labels [no_human,human], the float32 val-derived five-crop threshold, and the validated ESP32-P4 mixed INT8/INT16 profile.\n"
            "Regenerate the model with the current training/export/quantization pipeline.")
    endif()

    file(SHA256 "${classifier_model}" classifier_actual_sha256)
    string(TOLOWER "${classifier_expected_sha256}" classifier_expected_sha256)
    string(TOLOWER "${classifier_actual_sha256}" classifier_actual_sha256)
    if (NOT classifier_actual_sha256 STREQUAL classifier_expected_sha256)
        message(FATAL_ERROR
            "Classifier SHA-256 does not match ${classifier_manifest}.\n"
            "The .espdl and manifest are from different runs; regenerate both with model/quantize_espdl.py.")
    endif()

    set(CLASSIFIER_FIVE_CROP_HUMAN_THRESHOLD
        "${classifier_human_threshold}"
        PARENT_SCOPE)
endfunction()

if (CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if (NOT DEFINED CLASSIFIER_MODEL)
        message(FATAL_ERROR "Pass -DCLASSIFIER_MODEL=<path-to-.espdl> in script mode.")
    endif()
    validate_classifier_model("${CLASSIFIER_MODEL}")
    message(STATUS "Classifier manifest validation passed: ${CLASSIFIER_MODEL}")
endif()
