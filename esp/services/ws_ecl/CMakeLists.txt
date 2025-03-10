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


# Component: ws_ecl
#####################################################
# Description:
# ------------
#    Cmake Input File for ws_ecl
#####################################################

project( ws_ecl )

set (    SRCS
         ws_ecl_plugin.cpp
         ws_ecl_service.cpp
         ws_ecl_wuinfo.cpp
    )

include_directories (
         ./../../esplib
         ./../../platform
         ./../../services
         ./../../services/common
         ./../../bindings
         ./../../bindings/SOAP/xpp
         ./../../bindings/http/client
         ./../../clients
         ./../../../system/mp
         ./../../../system/jlib
         ./../../../common/environment
         ./../../../system/hrpc
         ./../../../system/xmllib
         ./../../../system/security/shared
         ./../../../system/security/securesocket
         ./../../../system/include
         ./../../../common/workunit
         ./../../../common/wuwebview
         ./../../../common/fileview2
         ./../../../dali/base
         ${HPCC_SOURCE_DIR}/common/thorhelper
         ${HPCC_SOURCE_DIR}/esp/smc/SMCLib
         ${HPCC_SOURCE_DIR}/dali/sasha
    )

ADD_DEFINITIONS( -D_USRDLL )

HPCC_ADD_LIBRARY( ws_ecl SHARED ${SRCS} )
add_dependencies( ws_ecl espscm )
install ( TARGETS ws_ecl RUNTIME DESTINATION ${EXEC_DIR} LIBRARY DESTINATION ${LIB_DIR} )
target_link_libraries ( ws_ecl
         jlib
         xmllib
         esphttp
         mp
         hrpc
         remote
         dalibase
         dllserver
         nbcd
         eclrtl
         deftype
         workunit
         wuwebview
         jhtree
         hql
         fileview2
         xmllib
         SMCLib
         ${COMMON_ESP_SERVICE_LIBS}
    )

if (NOT CONTAINERIZED)
    target_link_libraries ( ws_ecl environment )
endif()

IF (USE_OPENSSL)
    target_link_libraries ( ws_ecl
    	securesocket
    )
ENDIF()
