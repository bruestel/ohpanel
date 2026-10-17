# The seed of the display design

`seed.HMI.gz` is an empty project of the Nextion Editor 1.68.1, gzipped (the
container is 7 MiB, nearly all of it zeros):

- device NX4832F035_011 (Discovery series), display turned to 270 degrees
  (horizontal, as the NSPanel mounts it), character encoding iso-8859-1;
- one page, `page0`, with one object of each type the design uses: text,
  scrolling text, number, button, picture, hotspot, touch area, slider,
  timer, variable,
  as the Editor makes them, with its default attributes;
- the Editor's default `Program.s`; no fonts, no pictures.

The design is built on it (`boards/nspanel/nextion/build.sh`): the pictures are
imported into it, then `boards/nspanel/nextion/ohpanel_hmi.py` adds the fonts,
builds every page from copies of the seed's objects (the Editor expects every
attribute of an object's type, in its order; copying the Editor's own is
how the recipe gets them right), writes all code and `Program.s`, and drops
`page0`.

`boards/nspanel/nextion/tools/make-seed.sh` makes it again by driving the Editor.
