mkdir -p build/generated
python scripts/embed_shaders.py \
    build/generated/shaders.h \
    compiledshaders/*.spv
