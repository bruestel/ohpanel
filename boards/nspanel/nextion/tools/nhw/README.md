# nhw: Nextion HMI container format

`nextion_hmi_binary.py` and `nextion_hmi_checksum.py` are copied unchanged
(apart from a relative import and the "codesslide" event marker, which upstream
does not list; without it the code lines of slider events parse as markers;
and relaxed checks of the mirrored directory and its checksum, which the Editor itself
sometimes leaves stale) from
[newmatik/nextion-hmi-writer](https://github.com/newmatik/nextion-hmi-writer),
MIT licence, see `LICENSE`. They read and write the Nextion Editor's `.HMI`
container byte for byte, including the page and directory checksums, for
Nextion Editor 1.68.1.

`../hmipatch.py` builds on them.
