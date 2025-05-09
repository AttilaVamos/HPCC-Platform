################################################################################
#    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
################################################################################


# Component: dalibase 

#####################################################
# Description:
# ------------
#    Cmake Input File for dalibase
#####################################################


project( dalibase ) 

set (    SRCS 
         daaudit.cpp 
         daclient.cpp 
         dacoven.cpp 
         dacsds.cpp 
         dadfs.cpp 
         dadiags.cpp 
         dafdesc.cpp 
         dameta.cpp
         danqs.cpp 
         dasds.cpp 
         dasess.cpp
         dastats.cpp
         dasubs.cpp 
         dautils.cpp
         sysinfologger.cpp
    )
    
set (    INCLUDES
         daaudit.hpp
         daclient.hpp
         dacoven.hpp
         dadfs.hpp
         dadiags.hpp
         dafdesc.hpp
         dameta.hpp
         danqs.hpp
         daqueue.hpp
         dasds.hpp
         dasess.hpp
         dastats.hpp
         dasubs.hpp
         dautils.hpp
         sysinfologger.hpp
    )   

set_source_files_properties(dasds.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

include_directories ( 
         ${HPCC_SOURCE_DIR}/common/deftype 
         ${HPCC_SOURCE_DIR}/fs/dafsclient
         ${HPCC_SOURCE_DIR}/dali/server 
         ${HPCC_SOURCE_DIR}/system/mp 
         ${HPCC_SOURCE_DIR}/system/include 
         ${HPCC_SOURCE_DIR}/system/jlib 
         ${HPCC_SOURCE_DIR}/rtl/include 
         ${HPCC_SOURCE_DIR}/system/security/shared
         ${HPCC_SOURCE_DIR}/system/security/cryptohelper
         ${HPCC_SOURCE_DIR}/testing/unittests
    )

ADD_DEFINITIONS( -D_USRDLL -DDALI_EXPORTS -DNULL_DALIUSER_STACKTRACE)

HPCC_ADD_LIBRARY( dalibase SHARED ${SRCS} ${INCLUDES} )

if(NOT PLUGIN)
    install(
        TARGETS dalibase
        RUNTIME DESTINATION ${EXEC_DIR}
        LIBRARY DESTINATION ${LIB_DIR}
    )
    target_link_libraries(
        dalibase 
        jlib
        mp 
        hrpc 
        dafsclient
        ${CppUnit_LIBRARIES}
    )
endif()
