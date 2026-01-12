sudo  su
cd /data/TestToolQuickRun
export LD_LIBRARY_PATH=/data/TestToolQuickRun/suanneng1688/lib:$LD_LIBRARY_PATH
export QT_PLUGIN_PATH=/data/TestToolQuickRun/suanneng1688/plugins

export QT_QPA_PLATFORM_PLUGIN_PATH=/data/TestToolQuickRun/suanneng1688/plugins/platforms
export QT_QPA_PLATFORM=linuxfb
export QT_QPA_FONTDIR=/data/TestToolQuickRun/suanneng1688/lib/fonts

export XDG_RUNTIME_DIR=/data/TestToolQuickRun/qtusing

./suannengQT &