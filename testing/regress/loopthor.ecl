/*##############################################################################

    Copyright (C) 2011 HPCC Systems.

    All rights reserved. This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
############################################################################## */

//nohthor
//nothor

#option ('newChildQueries', true)

namesRecord :=
            RECORD
string20        surname;
string10        forename;
integer2        age := 25;
            END;

namesTable := dataset('x',namesRecord,FLAT);

namesTable2 := dataset([
        {'Halliday','Gavin',31},
        {'Halliday','Liz',30},
        {'Salter','Abi',10},
        {'X','Z'}], namesRecord);

two := 2 : stored('two');

//Case 1 fixed number of iterations
output(loop(namesTable2, 10, project(rows(left), transform(namesRecord, self.age := left.age*two; self := left))));

output(loop(namesTable2, 3, rows(left) & rows(left)));

//Case 2: fixed number of iterations with row filter
output(loop(namesTable2, 10, left.age <= 60, project(rows(left), transform(namesRecord, self.age := left.age*two; self := left))));

// ** case 3 ** global loop test

// ** case 4 ** global loop test

//case 5: a row filter
output(loop(namesTable2, left.age < 100, project(rows(left), transform(namesRecord, self.age := left.age*two; self := left))));



// Same as above except also using counter in various places.

// ** case 1 ** global loop test, although not based on dataset (counter <= 10)

//Case 2: fixed number of iterations with row filter
output(loop(namesTable2, 10, left.age * counter <= 200, project(rows(left), transform(namesRecord, self.age := left.age*two; self := left))));

// ** case 3 ** global loop test

// ** case 4 ** global loop test

loopBody(dataset(namesRecord) ds, unsigned4 c) :=
        project(ds, transform(namesRecord, self.age := left.age*c; self := left));

//case 5: a row filter
output(loop(namesTable2, left.age < 100, loopBody(rows(left), counter)));
