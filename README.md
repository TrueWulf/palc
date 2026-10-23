# palc (**pal**itra in **C**)

wallpaper palette generator, in C, no dependencies beyond imlib2.

reads an image, extracts its dominant colors with k-means clustering
and prints them as a terminal table, json, plain hex or osc escape
sequences. can also render template files and recolor running
terminals live.

## build

    make
    make install PREFIX=$HOME/.local

needs imlib2 and its headers (imlib2-devel / libimlib2-dev).

## usage

    palc              palette of the current wallpaper
    palc -n 8         exactly 8 colors
    palc -m           one hex per line, for scripts
    palc -j           json output
    palc -e           osc sequences for live terminal recolor
    palc -R           reset terminals to the saved original theme
    palc -T tpl -o out    render template with {color0}..{color31},
                          {background}, {foreground}
    palc -h           help
    palc -v           version
    
without an argument palc finds the current wallpaper itself
($WALLPAPER, wallpaper daemons, desktop settings, common config files).

the auto-theming hook (palc-hook) lives in your wallpaper picker:
see ~/.config/palc/rc to enable or disable it. it renders
~/.config/palc/templates/*.tpl (ships havoc.cfg.tpl and foot.ini.tpl)
and applies them: havoc gets a live config reload, foot is written to
~/.config/foot/foot.ini so every new window starts themed.

## license

isc, see [LICENSE](LICENSE)
