# Polyfem Solvers
# License: MIT

if(TARGET polysolve)
    return()
endif()

message(STATUS "Third-party: creating target 'polysolve'")


include(FetchContent)
FetchContent_Declare(
    polysolve
    GIT_REPOSITORY https://github.com/njsyw1997/polysolve.git
    GIT_TAG 38fe771e6b91f4fe3416980f62dc16fc96e360e4
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(polysolve)
