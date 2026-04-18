#!/bin/bash

set -eu

mkdir -p Release
(
    cd Release
    cmake .. -DCMAKE_BUILD_TYPE=Release
    cmake --build .
)

export WX_BODYSLIDE_DATA_DIR="$HOME/Games/Skyrim Special Edition/Data/CalienteTools/Bodyslide"
export WX_OUTFITSTUDIO_DATA_DIR="$WX_BODYSLIDE_DATA_DIR"
export DRI_PRIME=1!

cp -xa res "$WX_BODYSLIDE_DATA_DIR"/
cp -xa Release/OutfitStudio "$WX_BODYSLIDE_DATA_DIR"/

./Release/BodySlide
