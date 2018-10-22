cc_library(
    name = 'libuscb',       
    srcs = [
        'uscb.cc'
    ]
)

cc_library(
    name = 'libdscb',       
    srcs = [
        'dscb.cc'
    ]
)

cc_library(
    name = 'liblsp',
    srcs = [
        'lsp.cc'
    ]
)

cc_binary(
    name = 'mp2mp',
    srcs = [
        'mp2mp.cc',
    ],
    deps = [
        '#gflags',
        '#glog',
        ':libuscb',
        ':libdscb',
        ':liblsp',
    ],

    extra_cppflags = [
        '-Wno-non-virtual-dtor',
        '-Wno-overloaded-virtual',
    ],

#    extra_linkflags = [
#        '-Wl,-rpath=.',
#    ],
)

#gen_rule(
#    name = 'configure',
#    cmd = 'TARGET_PATH=`dirname $TARGET`;cp $${TARGET_PATH:16}/mp2mp.conf $$TARGET_PATH;cp $${TARGET_PATH:16}/script/* $$TARGET_PATH',
#    outs = ['mp2mp.conf'],
#    deps = [
#        ':RankingService'
#    ]
#)
