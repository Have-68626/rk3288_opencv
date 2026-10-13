# ═══════════════════════════════════════════════════════════════════
# face_infer.cmake — 人脸推理源文件分组变量
# ═══════════════════════════════════════════════════════════════════

set(RK_FACE_INFER_CORE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/ArcFaceEmbedder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/BioAuth.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FaceAlign.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FaceInferencePipeline.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FaceInferOutcomeJson.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/FaceInferStages.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/MotionDetector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/Storage.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/YoloFaceDetector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/Engine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/ModelRegistry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/VideoManager.cpp
)

set(RK_ADAPTER_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/ArcFaceAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/CascadeAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/DnnSsdAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/LbphAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/MobileFaceNetAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/RetinaFaceAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/SFaceAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/YoloFaceAdapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/src/adapters/YuNetAdapter.cpp
)
