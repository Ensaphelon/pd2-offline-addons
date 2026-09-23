/* The game's own overlay art, decoded once and carried as DC6.

   D2Win's loader only understands DC6, and every light effect in the archive is DCC — so the
   DCC is decoded outside the game (scratch/beam/build_dc6.py, ported from OpenDiablo2's reader)
   and the frames are re-coded as a DC6 that never came from a file. D2Cmp's InitCellFile takes
   it exactly the same way, which is how BH shows images of its own. */
#ifndef PD2HOLYGRAIL_ART_H
#define PD2HOLYGRAIL_ART_H

/* `_foot` is where the light stands, measured from the left edge — the middle for the upright
   ones, and for a leaning one the place its bottom ended up. */

/* data\global\overlays\HoradricLightBeam.dcc — the shaft of light the Horadric quest shines. */
extern const int art_beam_frames, art_beam_width, art_beam_height, art_beam_foot;
extern const unsigned int art_beam_size;
extern const unsigned char art_beam[];

/* data\global\overlays\LIGHTJET.dcc — rays fanning up out of the ground and fading again. */
extern const int art_jet_frames, art_jet_width, art_jet_height, art_jet_foot;
extern const unsigned int art_jet_size;
extern const unsigned char art_jet[];

/* The same beam, three times as wide and leant over — the game ships no slanted shaft, because
   the ones in its dungeons are painted into the floor tiles rather than animated. */
extern const int art_slant_frames, art_slant_width, art_slant_height, art_slant_foot;
extern const unsigned int art_slant_size;
extern const unsigned char art_slant[];

#endif
