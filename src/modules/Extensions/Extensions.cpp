#include <Extensions.hpp>

#include <Downloader.hpp>
#include <YouTube.hpp>
#include <LastFM.hpp>
#include <Radio.hpp>
#include <ProstoPleer.hpp>
#ifdef USE_MPRIS2
	#include <MPRIS2.hpp>
#endif

Extensions::Extensions() :
	Module( "Extensions" ),
	downloader( QImage( ":/downloader" ) ), youtube( QImage( ":/youtube" ) ), radio( QImage( ":/radio" ) ), lastfm( QImage( ":/lastfm" ) ), prostopleer( QImage( ":/prostopleer" ) )
{
	downloader.setText( "Path", ":/downloader" );
	youtube.setText( "Path", ":/youtube" );
	radio.setText( "Path", ":/radio" );
	lastfm.setText( "Path", ":/lastfm" );
	prostopleer.setText( "Path", ":/prostopleer" );

	init( "YouTube/ShowAdditionalInfo", false );
	init( "YouTube/youtubedl", QString() );
	init( "YouTube/ItagList", QStringList() << "22" << "43" << "18" );

	init( "LastFM/DownloadCovers", true );
	init( "LastFM/AllowBigCovers", false );
	init( "LastFM/UpdateNowPlayingAndScrobble", false );
	init( "LastFM/Login", QString() );
	init( "LastFM/Password", QString() );

#if USE_MPRIS2
	init( "MPRIS2/Enabled", true );
	init( "MPRIS2/ExportCovers", false );
#endif
}

QList< Extensions::Info > Extensions::getModulesInfo( const bool ) const
{
	QList< Info > modulesInfo;
	modulesInfo += Info( DownloaderName, QMPLAY2EXTENSION, downloader );
	modulesInfo += Info( YouTubeName, QMPLAY2EXTENSION, youtube );
	modulesInfo += Info( LastFMName, QMPLAY2EXTENSION, lastfm );
	modulesInfo += Info( RadioName, QMPLAY2EXTENSION, radio );
	modulesInfo += Info( ProstoPleerName, QMPLAY2EXTENSION, prostopleer );
#if USE_MPRIS2
	modulesInfo += Info( MPRIS2Name, QMPLAY2EXTENSION );
#endif
	return modulesInfo;
}
void *Extensions::createInstance( const QString &name )
{
	if ( name == DownloaderName )
		return new Downloader( *this );
	else if ( name == YouTubeName )
		return new YouTube( *this );
	else if ( name == LastFMName )
		return static_cast< QMPlay2Extensions * >( new LastFM( *this ) );
	else if ( name == RadioName )
		return static_cast< QMPlay2Extensions * >( new Radio( *this ) );
	else if ( name == ProstoPleerName )
		return static_cast< QMPlay2Extensions * >( new ProstoPleer( *this ) );
#ifdef USE_MPRIS2
	else if ( name == MPRIS2Name )
		return static_cast< QMPlay2Extensions * >( new MPRIS2( *this ) );
#endif
	return NULL;
}

Extensions::SettingsWidget *Extensions::getSettingsWidget()
{
	return new ModuleSettingsWidget( *this );
}

QMPLAY2_EXPORT_PLUGIN( Extensions )

/**/

#include "LineEdit.hpp"

#include <QCryptographicHash>
#include <QGridLayout>
#include <QListWidget>
#include <QToolButton>
#include <QFileDialog>
#include <QGroupBox>
#include <QCheckBox>
#include <QLabel>

ModuleSettingsWidget::ModuleSettingsWidget( Module &module ) :
	Module::SettingsWidget( module )
{
	QGridLayout *layout;

#ifdef USE_MPRIS2
	MPRIS2B = new QGroupBox( tr( "Obsługa programu przez interfejs MPRIS2" ) );
	MPRIS2B->setCheckable( true );
	MPRIS2B->setChecked( sets().getBool( "MPRIS2/Enabled" ) );

	exportCoversB = new QCheckBox( tr( "Wydobywaj okładki z plików multimedialnych" ) );
	exportCoversB->setChecked( sets().getBool( "MPRIS2/ExportCovers" ) );

	layout = new QGridLayout( MPRIS2B );
	layout->addWidget( exportCoversB );
	layout->setMargin( 2 );
#endif

	/**/

	QGroupBox *youTubeB = new QGroupBox( "YouTube" );

	additionalInfoB = new QCheckBox( tr( "Pokazuj dodatkowe informacje wyszukiwania" ) );
	additionalInfoB->setChecked( sets().getBool( "YouTube/ShowAdditionalInfo" ) );

	QLabel *youtubedlL = new QLabel( tr( "Ścieżka do programu 'youtube-dl'" ) + ": " );

	youtubedlE = new LineEdit;
	youtubedlE->setPlaceholderText( "youtube-dl" );
	youtubedlE->setText( sets().getString( "YouTube/youtubedl" ) );

	youtubedlBrowseB = new QToolButton;
	youtubedlBrowseB->setIcon( QMPlay2Core.getIconFromTheme( "folder-open" ) );
	youtubedlBrowseB->setToolTip( tr( "Przeglądaj" ) );
	connect( youtubedlBrowseB, SIGNAL( clicked() ), this, SLOT( browseYoutubedl() ) );

	QLabel *itagL = new QLabel( tr( "Kolejność wybierania domyślnej jakości filmu" ) + ": " );

	itagLW = new QListWidget;
	itagLW->setDragDropMode( QListWidget::InternalMove );
	itagLW->setSelectionMode( QListWidget::ExtendedSelection );
	ItagNames itagNames = YouTube::getItagNames( sets().get( "YouTube/ItagList" ).toStringList() );
	while ( !itagNames.first.isEmpty() )
	{
		QListWidgetItem *lWI = new QListWidgetItem( itagLW );
		lWI->setText( itagNames.first.takeFirst() );
		lWI->setData( Qt::UserRole, itagNames.second.takeFirst() );
	}

	layout = new QGridLayout( youTubeB );
	layout->addWidget( additionalInfoB, 0, 0, 1, 3 );
	layout->addWidget( youtubedlL, 1, 0, 1, 1 );
	layout->addWidget( youtubedlE, 1, 1, 1, 1 );
	layout->addWidget( youtubedlBrowseB, 1, 2, 1, 1 );
	layout->addWidget( itagL, 2, 0, 1, 3 );
	layout->addWidget( itagLW, 3, 0, 1, 3 );
	layout->setMargin( 2 );

	/**/

	QGroupBox *lastFMB = new QGroupBox( "LastFM" );

	downloadCoversB = new QCheckBox( tr( "Pobieraj okładki" ) );
	downloadCoversB->setChecked( sets().getBool( "LastFM/DownloadCovers" ) );

	allowBigCovers = new QCheckBox( tr( "Zezwalaj na pobieranie dużych okładek" ) );
	allowBigCovers->setChecked( sets().getBool( "LastFM/AllowBigCovers" ) );

	updateNowPlayingAndScrobbleB = new QCheckBox( tr( "Scrobbluj" ) );
	updateNowPlayingAndScrobbleB->setChecked( sets().getBool( "LastFM/UpdateNowPlayingAndScrobble" ) );

	loginE = new LineEdit;
	loginE->setPlaceholderText( tr( "Nazwa użytkownika" ) );
	loginE->setText( sets().getString( "LastFM/Login" ) );

	passwordE = new LineEdit;
	passwordE->setEchoMode( LineEdit::Password );
	passwordE->setPlaceholderText( sets().getString( "LastFM/Password" ).isEmpty() ? tr( "Hasło" ) : tr( "Poprzednio ustawione hasło" ) );
	connect( passwordE, SIGNAL( textEdited( const QString & ) ), this, SLOT( passwordEdited() ) );

	allowBigCovers->setEnabled( downloadCoversB->isChecked() );
	loginPasswordEnable( updateNowPlayingAndScrobbleB->isChecked() );

	connect( downloadCoversB, SIGNAL( toggled( bool ) ), allowBigCovers, SLOT( setEnabled( bool ) ) );
	connect( updateNowPlayingAndScrobbleB, SIGNAL( toggled( bool ) ), this, SLOT( loginPasswordEnable( bool ) ) );

	layout = new QGridLayout( lastFMB );
	layout->addWidget( downloadCoversB );
	layout->addWidget( allowBigCovers );
	layout->addWidget( updateNowPlayingAndScrobbleB );
	layout->addWidget( loginE );
	layout->addWidget( passwordE );
	layout->setMargin( 2 );

	/**/


	QGridLayout *mainLayout = new QGridLayout( this );
	mainLayout->setProperty( "NoVHSpacer", true );
#ifdef USE_MPRIS2
	mainLayout->addWidget( MPRIS2B );
#endif
	mainLayout->addWidget( youTubeB );
	mainLayout->addWidget( lastFMB );
}

void ModuleSettingsWidget::browseYoutubedl()
{
	const QString filePath = QFileDialog::getOpenFileName( this, tr( "Wybierz program 'youtube-dl'" ), QMPlay2Core.getQMPlay2Dir() );
	if ( !filePath.isEmpty() )
		youtubedlE->setText( filePath );
}
void ModuleSettingsWidget::loginPasswordEnable( bool checked )
{
	loginE->setEnabled( checked );
	passwordE->setEnabled( checked );
}
void ModuleSettingsWidget::passwordEdited()
{
	passwordE->setProperty( "edited", true );
}

void ModuleSettingsWidget::saveSettings()
{
#ifdef USE_MPRIS2
	sets().set( "MPRIS2/Enabled", MPRIS2B->isChecked() );
	sets().set( "MPRIS2/ExportCovers", exportCoversB->isChecked() );
#endif

	sets().set( "YouTube/ShowAdditionalInfo", additionalInfoB->isChecked() );
	sets().set( "YouTube/youtubedl", youtubedlE->text() );
	QStringList itags;
	for ( int i = 0 ; i < itagLW->count() ; ++i )
		itags += itagLW->item( i )->data( Qt::UserRole ).toString();
	sets().set( "YouTube/ItagList", itags );

	sets().set( "LastFM/DownloadCovers", downloadCoversB->isChecked() );
	sets().set( "LastFM/AllowBigCovers", allowBigCovers->isChecked() );
	sets().set( "LastFM/UpdateNowPlayingAndScrobble", updateNowPlayingAndScrobbleB->isChecked() && !loginE->text().isEmpty() );
	sets().set( "LastFM/Login", loginE->text() );
	if ( loginE->text().isEmpty() )
		sets().set( "LastFM/Password", QString() );
	else if ( !passwordE->text().isEmpty() && passwordE->property( "edited" ).toBool() )
		sets().set( "LastFM/Password", QString( QCryptographicHash::hash( passwordE->text().toUtf8(), QCryptographicHash::Md5 ).toHex() ) );
}
