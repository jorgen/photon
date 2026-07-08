# 3rdparty package definitions. Included by CmDepFetch (and CmDepFetchSetup in
# script mode). CmDepFetchPackage auto-declares, per dep: PHOTON_<DEP>_{VERSION,URL,SHA256}
# (CACHE STRING overrides, -D wins) and PHOTON_USE_SYSTEM_<DEP> (option; ON => consume
# a pre-built copy via find_package instead of fetching). The PHOTON_ prefix is derived
# from the project() name (PROJECT_NAME).

CmDepFetchPackage(vio 47615ee
    https://github.com/jorgen/vio/archive/47615eeead17b20359c0e412b87181532e383a3e.tar.gz
    SHA256=91f3d4d65d01672aa53771ab27d06e5b65beff3527e0528a4b9372e8c643814d)

CmDepFetchPackage(structify b8fec28d24
    https://github.com/jorgen/structify/archive/b8fec28d2449640e4c5668a59c736555e50aee81.tar.gz
    SHA256=9aa952d2f93e2762ea4e1537eb5f409a77c933fa1a79cc8d276ec113b800bde8)

CmDepFetchPackage(doctest 2.4.12
    https://github.com/doctest/doctest/archive/v2.4.12.tar.gz
    SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70)
