xlnxvideoscale_sources = [
  'gstxlnxvideoscale.c',
]

gstxlnxvideoscale = library('gstxlnxvideoscale',
    xlnxvideoscale_sources,
    c_args : gst_plugins_bad_args,
    include_directories : [configinc],
    dependencies : [gstbase_dep, gstvideo_dep, gstallocators_dep],
    install : true,
    install_dir : plugins_install_dir,
)
