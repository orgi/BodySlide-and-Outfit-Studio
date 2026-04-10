#!/bin/bash

(
    cd Release
    cmake --build . --config Release
)

export WX_BODYSLIDE_DATA_DIR="$HOME/Games/Skyrim Special Edition/Data/CalienteTools/Bodyslide"
export WX_OUTFITSTUDIO_DATA_DIR="$WX_BODYSLIDE_DATA_DIR"
export DRI_PRIME=1!
./Release/BodySlide
