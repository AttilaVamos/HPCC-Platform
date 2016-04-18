/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

import Std.File AS FileServices;

Layout_Person := RECORD
  UNSIGNED1 PersonID;
  STRING15  FirstName;
  STRING25  LastName;
END;

allPeople := DATASET([ {1,'Fred','Smith'},
                       {2,'Joe','Blow'},
                       {3,'Jane','Smith'}],Layout_Person);

//  Outputs  ---
output(allPeople, , '~persons', OVERWRITE);

SrcIP := 'localhost';
File := 'persons';
SourceFile := '~::' + File;

ClusterName := 'mythor';
DestFile1 := '/var/lib/HPCCSystems/mydropzone/' + File;
ESPportIP := 'http://127.0.0.1:8010/FileSpray';

FileServices.Despray(   SourceFile,
                        SrcIp,
                        destinationPath := DestFile1,
                        ALLOWOVERWRITE := true);

DestFile2 := '/var/lib/HPCCSystems/mydropzone/./' + File;
FileServices.Despray(   SourceFile,
                        SrcIp,
                        destinationPath := DestFile2,
                        ALLOWOVERWRITE := true);

DestFile3 := '/var/lib/HPCCSystems/mydropzone/../' + File;
FileServices.Despray(   SourceFile,
                        SrcIp,
                        destinationPath := DestFile3,
                        ALLOWOVERWRITE := true);

DestFile4 := '/var/lib/HPCCSystems/mydropzona/' + File;
FileServices.Despray(   SourceFile,
                        SrcIp,
                        destinationPath := DestFile4,
                        ALLOWOVERWRITE := true);

DestFile5 := '/var/lib/HPCCSystems/' + File;
FileServices.Despray(   SourceFile,
                        SrcIp,
                        destinationPath := DestFile4,
                        ALLOWOVERWRITE := true);

