# Outfit Modularizer

The modularizer will support you to quickly create (more) modular versions of an existing outfit. It will:
1. Create the reference nifs for the extracted shapes in BodySlide/ShapeData (making sure that all the sliders still work)
2. Add the new reference nifs to the CBBE and 3BA outfit groups
3. Create or update a new `<my plugin>_modular.esp`
    * Create new records for the new parts created from extracting shapes
    * Update existing records i.e. for the 'remainder' nif

## Prerequesits

Some custom clothing / armor plugins that are not modular enough but are already internally split into separate shapes.
The modularizer does not support splitting shapes into new shapes; but it supports moving shapes into new nifs.

## How it works

1. There is a new button 'Modularize' in OutfitStudio in the 'Shapes' tab on the right
2. After selecting at least one shape, you click on 'Modularize' to extract those shapes into new parts
3. After a short time a dialog will open where you can specify and see the following details:
   1. Name of the original plugin (auto-detected) and the new or to-be-updated modular plugin
   2. Names of the new and the remainder parts
   3. Slots to be used for the new and the remainder parts
   4. Comments in case the selected slots are already in use by other parts (possibly intentionally!)
