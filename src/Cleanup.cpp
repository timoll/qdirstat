/*
 *   File name: Cleanup.cpp
 *   Summary:	QDirStat classes to reclaim disk space
 *   License:	GPL V2 - See file LICENSE for details.
 *
 *   Author:	Stefan Hundhammer <Stefan.Hundhammer@gmx.de>
 */


#include <QApplication>
#include <QRegExp>
#include <QProcessEnvironment>
#include <QFileInfo>

#include "Cleanup.h"
#include "FileInfo.h"
#include "DirTree.h"
#include "DirSaver.h"
#include "OutputWindow.h"
#include "Process.h"
#include "Logger.h"
#include "Exception.h"


#define SIMULATE_COMMAND	1
#define WAIT_TIMEOUT_MILLISEC	30000

using namespace QDirStat;


Cleanup::Cleanup( QString   command,
		  QString   title,
		  QObject * parent ):
    QAction( title, parent ),
    _command( command ),
    _title( title )
{
    _active		   = true;
    _worksForDir	   = true;
    _worksForFile	   = true;
    _worksForDotEntry	   = false;
    _recurse		   = false;
    _askForConfirmation	   = false;
    _refreshPolicy	   = RefreshThis;
    _outputWindowPolicy	   = ShowAfterTimeout;
    _outputWindowTimeout   = 500;
    _outputWindowAutoClose = false;

    QAction::setEnabled( true );
}


void Cleanup::setTitle( const QString &title )
{
    _title = title;
    QAction::setText( _title );
}


void Cleanup::setIcon( const QIcon & icon )
{
    _icon = icon;
    QAction::setIcon( icon );
}


bool Cleanup::worksFor( FileInfo *item ) const
{
    if ( ! _active || ! item )
	return false;

    if	( item->isDotEntry() )	return worksForDotEntry();
    if	( item->isDir() )	return worksForDir();

    return worksForFile();
}


void Cleanup::execute( FileInfo *item, OutputWindow * outputWindow )
{
    if ( worksFor( item ) )
    {
	DirTree * tree = item->tree();

	executeRecursive( item, outputWindow );

	switch ( _refreshPolicy )
	{
	    case NoRefresh:
		// Do nothing (by definition).
		break;

	    case RefreshThis:
	    case RefreshParent:
		// Done from CleanupCollection::execute() via a Refresher
		// object that is triggered by the
		// OutputWindow::lastProcessFinished() signal.
		//
		// Nothing left to do here.
		break;

	    case AssumeDeleted:

		// Assume the cleanup action has deleted the item.
		// Modify the DirTree accordingly.

		tree->deleteSubtree( item );
		break;
	}
    }
}


void Cleanup::executeRecursive( FileInfo *item, OutputWindow * outputWindow )
{
    if ( worksFor( item ) )
    {
	if ( _recurse )
	{
	    // Recurse into all subdirectories.

	    FileInfo * subdir = item->firstChild();

	    while ( subdir )
	    {
		if ( subdir->isDir() )
		{
		    /**
		     * Recursively execute in this subdirectory, but only if it
		     * really is a directory: File children might have been
		     * reparented to the directory (normally, they reside in
		     * the dot entry) if there are no real subdirectories on
		     * this directory level.
		     **/
		    executeRecursive( subdir, outputWindow );
		}
		subdir = subdir->next();
	    }
	}


	// Perform cleanup for this directory.

	runCommand( item, _command, outputWindow );
    }
}


const QString Cleanup::itemDir( const FileInfo *item ) const
{
    QString dir = item->url();

    if ( ! item->isDir() && ! item->isDotEntry() )
    {
	dir.replace ( QRegExp ( "/[^/]*$" ), "" );
    }

    return dir;
}


QString Cleanup::cleanTitle() const
{
    // Use the cleanup action's title, if possible.

    QString title = _title;

    if ( title.isEmpty() )
	title = _command;

    // Get rid of any "&" characters in the text that denote keyboard
    // shortcuts in menus.
    title.replace( QRegExp( "&" ), "" );

    return title;
}


QString Cleanup::expandVariables( const FileInfo * item,
				  const QString	 & unexpanded ) const
{
    QString expanded = expandDesktopSpecificApps( unexpanded );
    QString dirName = "";

    if ( item->isDir() )
	dirName = item->url();
    else if ( item->parent() )
	dirName = item->parent()->url();

    expanded.replace( "%p", quoted( escaped( item->url()  ) ) );
    expanded.replace( "%n", quoted( escaped( item->name() ) ) );

    if ( ! dirName.isEmpty() )
	expanded.replace( "%d", quoted( escaped( dirName ) ) );

    // logDebug() << "Expanded: \"" << expanded << "\"" << endl;
    return expanded;
}


QString Cleanup::quoted( const QString & unquoted) const
{
    return "'" + unquoted + "'";
}


QString Cleanup::escaped( const QString & unescaped ) const
{
    QString escaped = unescaped;

    // Escape single quote characters (') in the string.
    //
    // While any sane person would expect this should be done with a backslash
    // in front of the single quote, i.e. \', this is not how shells do it.
    // Instead, you have to terminate the string with one single quote, then
    // put the single quote in a new quoted string that, but this time using
    // double quotes, and finally reopen the original string with another
    // single quote.
    //
    // Thus, 'Don't do this' becomes 'Don'"'"'t do this'.
    //
    // This does not exactly become any prettier with the C compiler requiring
    // a backslash for an embedded double quote.

    escaped.replace( "'", "'\\''" );

    return escaped;
}


QString Cleanup::chooseShell( OutputWindow * outputWindow ) const
{
    QString errMsg;
    QString shell = this->shell();

    if ( ! shell.isEmpty() )
    {
	logDebug() << "Using custom shell " << shell << endl;

	if ( ! isExecutable( shell ) )
	{
	    errMsg = tr( "ERROR: Shell %1 is not executable" ).arg( shell );
	    shell = defaultShell();

	    if ( ! shell.isEmpty() )
		errMsg += "\n" + tr( "Using fallback %1" ).arg( shell );
	}
    }

    if ( shell.isEmpty() )
    {
	shell = defaultShell();
	logDebug() << "No custom shell configured - using " << shell << endl;
    }

    if ( ! errMsg.isEmpty() )
    {
	outputWindow->show(); // Show error regardless of user settings
	outputWindow->addStderr( errMsg );
    }

    return shell;
}


void Cleanup::runCommand( const FileInfo * item,
			  const QString	 & command,
			  OutputWindow	 * outputWindow ) const
{
    QString shell = chooseShell( outputWindow );

    if ( shell.isEmpty() )
    {
	outputWindow->show(); // Regardless of user settings
	outputWindow->addStderr( tr( "No usable shell - aborting cleanup action" ) );
	logError() << "ERROR: No usable shell" << endl;
	return;
    }

    QString cleanupCommand( expandVariables( item, command ));
    Process * process = new Process( parent() );
    CHECK_NEW( process );

    process->setProgram( shell );
    process->setArguments( QStringList() << "-c" << cleanupCommand );
    process->setWorkingDirectory( itemDir( item ) );
    // logDebug() << "New process \"" << process << endl;

    outputWindow->addProcess( process );

    // The CleanupCollection will take care about refreshing if this is
    // configured for this cleanup.
}


QMap<int, QString> Cleanup::refreshPolicyMapping()
{
    QMap<int, QString> mapping;

    mapping[ NoRefresh	   ] = "NoRefresh";
    mapping[ RefreshThis   ] = "RefreshThis";
    mapping[ RefreshParent ] = "RefreshParent";
    mapping[ AssumeDeleted ] = "AssumeDeleted";

    return mapping;
}


QMap<int, QString> Cleanup::outputWindowPolicyMapping()
{
    QMap<int, QString> mapping;

    mapping[ ShowAlways	       ] = "ShowAlways";
    mapping[ ShowIfErrorOutput ] = "ShowIfErrorOutput";
    mapping[ ShowAfterTimeout  ] = "ShowAfterTimeout";
    mapping[ ShowNever	       ] = "ShowNever";

    return mapping;
}


bool Cleanup::isExecutable( const QString & programName )
{
    if ( programName.isEmpty() )
	return false;

    QFileInfo fileInfo( programName );
    return fileInfo.isExecutable();
}


QString Cleanup::loginShell()
{
    static bool cached = false;
    static QString shell;

    if ( ! cached )
    {
	cached = true;
	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	shell = env.value( "SHELL", "" );

	if ( ! isExecutable( shell ) )
	{
	    logError() << "ERROR: Shell \"" << shell << "\" is not executable" << endl;
	    shell = "";
	}
    }

    return shell;
}


const QStringList & Cleanup::defaultShells()
{
    static bool cached = false;
    static QStringList shells;

    if ( ! cached )
    {
	cached = true;
	QStringList candidates;
	candidates << loginShell() << "/bin/bash" << "/bin/sh";

	foreach ( const QString & shell, candidates )
	{
	    if ( isExecutable( shell ) )
		 shells << shell;
	    else if ( ! shell.isEmpty() )
	    {
		logWarning() << "Shell " << shell << " is not executable" << endl;
	    }
	}

	if ( ! shells.isEmpty() )
	    logDebug() << "Default shell: " << shells.first() << endl;
    }

    if ( shells.isEmpty() )
	logError() << "ERROR: No usable shell" << endl;

    return shells;
}


QString Cleanup::defaultShell()
{
    return defaultShells().isEmpty() ? QString() : defaultShells().first();
}


const QMap<QString, QString> & Cleanup::desktopSpecificApps()
{
    static QMap<QString, QString> apps;

    if ( apps.isEmpty() )
    {
	QString desktop = QString::fromUtf8( qgetenv( "QDIRSTAT_DESKTOP" ) );

	if ( desktop.isEmpty() )
	     desktop = QString::fromUtf8( qgetenv( "XDG_CURRENT_DESKTOP" ) );
	else
	{
	    logDebug() << "Overriding $XDG_CURRENT_DESKTOP with $QDIRSTAT_DESKTOP (\""
		       << desktop << "\")" << endl;
	}

	if ( desktop.isEmpty() )
	{
	    logWarning() << "$XDG_CURRENT_DESKTOP is not set - using fallback apps" << endl;
	    apps = fallbackApps();
	}
	else
	{
	    logInfo() << "Detected desktop \"" << desktop << "\"" << endl;
	    desktop = desktop.toLower();

	    if ( desktop == "kde" )
	    {
		// KDE konsole misbehaves in every way possible:
		//
		// It cannot be started in the background from a cleanup action,
		// it will terminate when QDirStat terminates,
		// and it doesn't give a shit about its current working directory.
		//
		// After having wasted four hours to get that thing to cooperate,
		// I simply don't care any more: The other terminals will get
		// the & added here rather than in the cleanup command line
		// where it would be appropriate. All this just because KDE
		// konsole doesn't comply with any standards whatsoever.

		apps[ "%terminal"    ] = "konsole --workdir %d";
		apps[ "%filemanager" ] = "konqueror --profile filemanagement";
	    }
	    else if ( desktop == "gnome" ||
		      desktop == "unity"   )
	    {
		apps[ "%terminal"    ] = "gnome-terminal &";
		apps[ "%filemanager" ] = "nautilus";
	    }
	    else if ( desktop == "xfce" )
	    {
		apps[ "%terminal"    ] = "xfce4-terminal &";
		apps[ "%filemanager" ] = "thunar";
	    }
	    else if ( desktop == "lxde" )
	    {
		apps[ "%terminal"    ] = "lxterminal &";
		apps[ "%filemanager" ] = "pcmanfm";
	    }
	    else if ( desktop == "enlightenment" )
	    {
		apps[ "%terminal"    ] = "eterm &";
		apps[ "%filemanager" ] = "xdg-open";
	    }

	    if ( apps.isEmpty() )
	    {
		logWarning() << "No mapping available for this desktop - using fallback apps" << endl;
		apps = fallbackApps();
	    }
	}

	for ( QMap<QString, QString>::const_iterator it = apps.constBegin();
	      it != apps.constEnd();
	      ++it )
	{
	    logInfo() << it.key() << " => \"" << it.value() << "\"" << endl;
	}
    }

    return apps;
}


const QMap<QString, QString> & Cleanup::fallbackApps()
{
    static QMap<QString, QString> apps;

    if ( apps.isEmpty() )
    {
#ifdef Q_OS_MAC
	apps[ "%terminal"    ] = "open -a Terminal.app .";
	apps[ "%filemanager" ] = "open";
#else
	apps[ "%terminal"    ] = "xterm";
	apps[ "%filemanager" ] = "xdg-open";
#endif
    }

    return apps;
}


QString Cleanup::expandDesktopSpecificApps( const QString & unexpanded ) const
{
    QString expanded = unexpanded;
    const QMap<QString, QString> & apps = desktopSpecificApps();

    for ( QMap<QString, QString>::const_iterator it = apps.constBegin();
	  it != apps.constEnd();
	  ++it )
    {
	expanded.replace( it.key(), it.value() );
    }

    return expanded;
}
