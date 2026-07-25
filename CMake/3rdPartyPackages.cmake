# 3rdparty package definitions. Included by CmDepFetch (and CmDepFetchSetup in
# script mode). CmDepFetchPackage auto-declares, per dep: PHOTON_<DEP>_{VERSION,URL,SHA256}
# (CACHE STRING overrides, -D wins) and PHOTON_USE_SYSTEM_<DEP> (option; ON => consume
# a pre-built copy via find_package instead of fetching). The PHOTON_ prefix is derived
# from the project() name (PROJECT_NAME).

CmDepFetchPackage(vio 7a8425a
    https://github.com/jorgen/vio/archive/7a8425ab7341b0685abddb833c57cbacf1735470.tar.gz
    SHA256=193140b975bf519a3be1de27a30c34884e0c1ce046b1f3e20a51ca09b7a150aa)

CmDepFetchPackage(structify b8fec28d24
    https://github.com/jorgen/structify/archive/b8fec28d2449640e4c5668a59c736555e50aee81.tar.gz
    SHA256=9aa952d2f93e2762ea4e1537eb5f409a77c933fa1a79cc8d276ec113b800bde8)

CmDepFetchPackage(doctest 2.4.12
    https://github.com/doctest/doctest/archive/v2.4.12.tar.gz
    SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70)

if (PHOTON_WITH_PRISM)
    CmDepFetchPackage(prism 1661b9a
        https://github.com/jorgen/prism/archive/1661b9acc03eb6dc0d4b69b75255ddf5aaa0a9d9.tar.gz
        SHA256=1180da88fff18a89952f7c391ed23ec8c59ef3672dbc66434799e5530e4156c0)
endif ()
