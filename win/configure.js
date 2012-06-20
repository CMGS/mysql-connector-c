// Configure.js
//
// Copyright (C) 2006 MySQL AB
// 
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; version 2 of the License.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

ForReading = 1;
ForWriting = 2;
ForAppending = 8;

try 
{
    var fso = new ActiveXObject("Scripting.FileSystemObject");

    var args = WScript.Arguments
    
    var default_comment = "Source distribution";
    var default_port = 3306;
    var actual_port = 0;

    var configfile = fso.CreateTextFile("win\\configure.data", true);
    for (i=0; i < args.Count(); i++)
    {
        var parts = args.Item(i).split('=');
        switch (parts[0])
        {
            case "__NT__":
            case "CYBOZU":
            case "EXTRA_DEBUG":
            case "ENABLED_DEBUG_SYNC":
            case "EMBED_MANIFESTS":
                    configfile.WriteLine("SET (" + args.Item(i) + " TRUE)");
                    break;
            case "COMPILATION_COMMENT":
                    default_comment = parts[1];
                    break;
            case "MYSQL_TCP_PORT":
                    actual_port = parts[1];
                    break;
        }
    }
    if (actual_port == 0)
	{
       // if we actually defaulted (as opposed to the pathological case of
       // --with-tcp-port=<MYSQL_TCP_PORT_DEFAULT> which might in theory
       // happen if whole batch of servers was built from a script), set
       // the default to zero to indicate that; we don't lose information
       // that way, because 0 obviously indicates that we can get the
       // default value from MYSQL_TCP_PORT. this seems really evil, but
       // testing for MYSQL_TCP_PORT==MYSQL_TCP_PORT_DEFAULT would make a
       // a port of MYSQL_TCP_PORT_DEFAULT magic even if the builder did not
       // intend it to mean "use the default, in fact, look up a good default
       // from /etc/services if you can", but really, really meant 3306 when
       // they passed in 3306. When they pass in a specific value, let them
       // have it; don't second guess user and think we know better, this will
       // just make people cross.  this makes the the logic work like this
       // (which is complicated enough):
       // 
       // - if a port was set during build, use that as a default.
       // 
       // - otherwise, try to look up a port in /etc/services; if that fails,
       //   use MYSQL_TCP_PORT_DEFAULT (at the time of this writing 3306)
       // 
       // - allow the MYSQL_TCP_PORT environment variable to override that.
       // 
       // - allow command-line parameters to override all of the above.
       // 
       // the top-most MYSQL_TCP_PORT_DEFAULT is read from win/configure.js,
       // so don't mess with that.
	   actual_port = default_port;
	   default_port = 0;
	}

    configfile.WriteLine("SET (COMPILATION_COMMENT \"" +
                         default_comment + "\")");

    configfile.WriteLine("SET (MYSQL_TCP_PORT_DEFAULT \"" + default_port + "\")");
    configfile.WriteLine("SET (MYSQL_TCP_PORT \"" + actual_port + "\")");

    configfile.Close();
    
    fso = null;

    WScript.Echo("done!");
}
catch (e)
{
    WScript.Echo("Error: " + e.description);
}

function GetValue(str, key)
{
    var pos = str.indexOf(key+'=');
    if (pos == -1) return null;
    pos += key.length + 1;
    var end = str.indexOf("\n", pos);
    if (str.charAt(pos) == "\"")
        pos++;
    if (str.charAt(end-1) == "\"")
        end--;
    return str.substring(pos, end);    
}

function GetVersion(str)
{
    var key = "AC_INIT([MySQL Server], [";
    var pos = str.indexOf(key);
    if (pos == -1) return null;
    pos += key.length;
    var end = str.indexOf("]", pos);
    if (end == -1) return null;
    return str.substring(pos, end);
}

function GetBaseVersion(version)
{
    var dot = version.indexOf(".");
    if (dot == -1) return null;
    dot = version.indexOf(".", dot+1);
    if (dot == -1) dot = version.length;
    return version.substring(0, dot);
}

function GetVersionId(version)
{
    var dot = version.indexOf(".");
    if (dot == -1) return null;
    var major = parseInt(version.substring(0, dot), 10);
    
    dot++;
    var nextdot = version.indexOf(".", dot);
    if (nextdot == -1) return null;
    var minor = parseInt(version.substring(dot, nextdot), 10);
    dot = nextdot+1;
    
    var stop = version.indexOf("-", dot);
    if (stop == -1) stop = version.length;
    var build = parseInt(version.substring(dot, stop), 10);
    
    var id = major;
    if (minor < 10)
        id += '0';
    id += minor;
    if (build < 10)
        id += '0';
    id += build;
    return id;
}
