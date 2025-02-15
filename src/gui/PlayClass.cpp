#include <PlayClass.hpp>

#include <QMPlay2_OSD.hpp>
#include <VideoThr.hpp>
#include <AudioThr.hpp>
#include <DemuxerThr.hpp>
#include <LibASS.hpp>
#include <Main.hpp>

#include <VideoFrame.hpp>
#include <Functions.hpp>
#include <Settings.hpp>
#include <SubsDec.hpp>
#include <Demuxer.hpp>
#include <Decoder.hpp>
#include <Reader.hpp>

#if QT_VERSION >= 0x040800
	#define USE_QRAWFONT
#endif

#include <QCoreApplication>
#ifdef USE_QRAWFONT
	#include <QRawFont>
#else
	#include <QFontDatabase>
#endif
#include <QInputDialog>
#include <QMessageBox>
#include <QTextCodec>
#include <QAction>
#include <QDebug>
#include <QDir>

#include <math.h>

#if defined Q_OS_WIN && !defined Q_OS_WIN64
	#include <QProgressBar>
	#include <QVBoxLayout>
	#include <QLabel>

	class UpdateFC : public QThread
	{
	public:
		UpdateFC( LibASS *ass ) :
			ass( ass )
		{
			start();
			if ( !wait( 500 ) )
			{
				QDialog d( QMPlay2GUI.mainW );
				d.setWindowTitle( qApp->applicationName() );
				QLabel l( QObject::tr( "Czekaj, trwa aktualizacja pamięci podręcznej czcionek" ) );
				QProgressBar p;
				p.setRange( 0, 0 );
				QVBoxLayout la( &d );
				la.addWidget( &l );
				la.addWidget( &p );
				d.open();
				d.setMinimumSize( d.size() );
				d.setMaximumSize( d.size() );
				do
					el.processEvents( QEventLoop::ExcludeUserInputEvents | QEventLoop::WaitForMoreEvents );
				while ( isRunning() );
			}
		}
	private:
		void run()
		{
			ass->initOSD();
			SetFileAttributesW( ( WCHAR * )QString( QDir::homePath() + "/fontconfig" ).utf16(), FILE_ATTRIBUTE_HIDDEN );
			el.wakeUp();
		}
		LibASS *ass;
		QEventLoop el;
	};
#endif

PlayClass::PlayClass() :
	demuxThr( NULL ), vThr( NULL ), aThr( NULL ),
#if defined Q_OS_WIN && !defined Q_OS_WIN64
	firsttimeUpdateCache( true ),
#endif
	ass( NULL ), osd( NULL )
{
	doSilenceBreak = doSilenceOnStart = false;

	maxThreshold = 60.0;
	vol = 1.0;

	quitApp = muted = reload = false;

	aRatioName = "auto";
	speed = subtitlesScale = zoom = 1.0;
	flip = 0;

	restartSeekTo = SEEK_NOWHERE;

	subtitlesStream = -1;
	videoSync = subtitlesSync = 0.0;
	videoEnabled = audioEnabled = subtitlesEnabled = true;

	Brightness = Saturation = Contrast = Hue = 0;

	connect( &timTerminate, SIGNAL( timeout() ), this, SLOT( timTerminateFinished() ) );
	connect( this, SIGNAL( aRatioUpdate( double ) ), this, SLOT( aRatioUpdated( double ) ) );
}

void PlayClass::play( const QString &_url )
{
	if ( !demuxThr )
	{
		if ( audioEnabled || videoEnabled )
		{
			audioStream = videoStream = subtitlesStream = -1;

			url = _url;
			demuxThr = new DemuxerThr( *this );
			demuxThr->minBuffSizeLocal = QMPlay2Core.getSettings().getInt( "AVBufferLocal" );
			demuxThr->minBuffSizeNetwork = QMPlay2Core.getSettings().getInt( "AVBufferNetwork" );
			demuxThr->playIfBuffered = QMPlay2Core.getSettings().getDouble( "PlayIfBuffered" );
			demuxThr->updateBufferedSeconds = QMPlay2Core.getSettings().getBool( "ShowBufferedTimeOnSlider" );

			connect( demuxThr, SIGNAL( load( Demuxer * ) ), this, SLOT( load( Demuxer * ) ) );
			connect( demuxThr, SIGNAL( finished() ), this, SLOT( demuxThrFinished() ) );

			if ( !QMPlay2Core.getSettings().getBool( "KeepZoom" ) )
				zoomReset();
			if ( !QMPlay2Core.getSettings().getBool( "KeepARatio" ) )
				emit resetARatio();
			if ( !QMPlay2Core.getSettings().getBool( "KeepSubtitlesDelay" ) )
				subtitlesSync = 0.0;
			if ( !QMPlay2Core.getSettings().getBool( "KeepSubtitlesScale" ) )
				subtitlesScale = 1.0;
			if ( !QMPlay2Core.getSettings().getBool( "KeepVideoDelay" ) )
				videoSync = 0.0;
			if ( !QMPlay2Core.getSettings().getBool( "KeepSpeed" ) )
				speed = 1.0;

			replayGain = 1.0;

			canUpdatePos = true;
			waitForData = paused = flushVideo = flushAudio = endOfStream = false;
			lastSeekTo = seekTo = pos = SEEK_NOWHERE;
			skipAudioFrame = audio_current_pts = frame_last_pts = frame_last_delay = audio_last_delay = 0.0;

			ignorePlaybackError = QMPlay2Core.getSettings().getBool( "IgnorePlaybackError" );

			choosenAudioLang = QMPlay2GUI.languages.key( QMPlay2Core.getSettings().getString( "AudioLanguage" ) );
			choosenSubtitlesLang = QMPlay2GUI.languages.key( QMPlay2Core.getSettings().getString( "SubtitlesLanguage" ) );

			if ( restartSeekTo >= 0.0 ) //jeżeli restart odtwarzania
			{
				seekTo = restartSeekTo;
				restartSeekTo = SEEK_NOWHERE;
			}
			else
				choosenAudioStream = choosenVideoStream = choosenSubtitlesStream = -1;

			demuxThr->start();
		}
	}
	else
	{
		newUrl = _url;
		stop();
	}
}
void PlayClass::stop( bool _quitApp )
{
	if ( stopPauseMutex.tryLock() )
	{
		quitApp = _quitApp;
		if ( isPlaying() )
		{
			if ( aThr && newUrl.isEmpty() )
				 aThr->silence();
			if ( isPlaying() )
			{
				timTerminate.start( TERMINATE_TIMEOUT );
				demuxThr->stop();
				fillBufferB = true;
			}
		}
		else
		{
			stopAVThr();
			clearPlayInfo();
			if ( quitApp )
				emit quit();
		}
		stopPauseMutex.unlock();
	}
}
void PlayClass::restart()
{
	if ( !url.isEmpty() )
	{
		restartSeekTo = pos;
		play( url );
	}
}

void PlayClass::chPos( double _pos, bool updateGUI )
{
	if ( !canUpdatePos )
		return;
	if ( ( updateGUI || pos == -1 ) && ( ( int )_pos != ( int )pos ) )
		emit updatePos( _pos );
	pos = _pos;
	lastSeekTo = SEEK_NOWHERE;
}

void PlayClass::togglePause()
{
	if ( stopPauseMutex.tryLock() )
	{
		if ( aThr && !paused )
			aThr->silence();
		paused = !paused;
		fillBufferB = true;
		if ( aThr && !paused )
			aThr->silence( true );
		stopPauseMutex.unlock();
	}
}
void PlayClass::seek( int pos )
{
	if ( pos < 0 )
		pos = 0;
	if ( lastSeekTo == pos )
		return;
	emit QMPlay2Core.seeked( lastSeekTo = seekTo = pos );
	fillBufferB = true;
	if ( aThr && paused )
		aThr->silence( true );
}
void PlayClass::chStream( const QString &s )
{
	if ( s.left( 5 ) == "audio" )
		choosenAudioStream = s.right( s.length() - 5 ).toInt();
	else if ( s.left( 5 ) == "video" )
		choosenVideoStream = s.right( s.length() - 5 ).toInt();
	else if ( s.left( 9 ) == "subtitles" )
		choosenSubtitlesStream = s.right( s.length() - 9 ).toInt();
	else if ( s.left( 8 ) == "fileSubs" )
	{
		int idx = s.right( s.length() - 8 ).toInt();
		if ( fileSubsList.count() > idx )
			loadSubsFile( fileSubsList[ idx ] );
	}
}
void PlayClass::setSpeed( double spd )
{
	if ( spd < 0.05 || spd > 100.0 )
		speed = 1.0;
	else
		speed = spd;
	speedMessageAndOSD();
}

bool PlayClass::isPlaying() const
{
	return demuxThr && demuxThr->isRunning();
}
#ifdef Q_OS_WIN
bool PlayClass::isNowPlayingVideo() const
{
	return !paused && vThr && vThr->isRunning();
}
#endif

void PlayClass::loadSubsFile( const QString &fileName )
{
	bool subsLoaded = false;
	if ( demuxThr && vThr && ass )
	{
		IOController< Reader > reader;
		if ( Reader::create( fileName, reader ) && reader->size() > 0 )
		{
			QByteArray fileData = reader->read( reader->size() );

			QTextCodec *codec = QTextCodec::codecForName( QMPlay2Core.getSettings().getByteArray( "SubtitlesEncoding" ) );
			if ( codec && codec->name() != "UTF-8" )
			{
				codec = QTextCodec::codecForUtfText( fileData, codec );
				if ( codec->name() != "UTF-8" )
					fileData = codec->toUnicode( fileData ).toUtf8();
			}

			sPackets.lock();

			vThr->destroySubtitlesDecoder();
			if ( !QMPlay2Core.getSettings().getBool( "KeepSubtitlesDelay" ) )
				subtitlesSync = 0.0;
			ass->closeASS();
			ass->clearFonts();

			bool loaded = false;
			QString fileExt = Functions::fileExt( fileName ).toLower();
			if ( fileExt == "ass" || fileExt == "ssa" )
			{
				/* Wczytywanie katalogu z czcionkami dla wybranego pliku napisów */
				const QString fontPath = Functions::filePath( fileName.mid( 7 ) );
				foreach ( const QString &fontFile, QDir( fontPath ).entryList( QStringList() << "*.ttf" << "*.otf", QDir::Files ) )
				{
					QFile f( fontPath + fontFile );
					if ( f.size() <= 0xA00000 /* 10MiB max */ && f.open( QFile::ReadOnly ) )
					{
						const QByteArray fontData = f.readAll();
						f.close();
#ifdef USE_QRAWFONT
						const QString fontName = QRawFont( fontData, 0.0 ).familyName();
						if ( !fontName.isEmpty() )
							ass->addFont( fontName.toUtf8(), fontData );
#else //For Qt older than 4.8
						const int fontID = QFontDatabase::addApplicationFontFromData( fontData );
						if ( fontID != -1 )
						{
							const QStringList fontFamilies = QFontDatabase::applicationFontFamilies( fontID );
							QFontDatabase::removeApplicationFont( fontID );
							if ( !fontFamilies.isEmpty() )
								ass->addFont( fontFamilies.first().toUtf8(), fontData );
						}
#endif
					}
				}

				ass->initASS( fileData );
				loaded = true;
			}
			else
			{
				SubsDec *subsdec = SubsDec::create( fileExt );
				if ( subsdec )
				{
					loaded = subsdec->toASS( fileData, ass, fps );
					delete subsdec;
				}
			}
			if ( loaded )
			{
				fileSubs = fileName;
				subtitlesStream = choosenSubtitlesStream = -2; //"subtitlesStream < -1" oznacza, że wybrano napisy z pliku
				if ( !fileSubsList.contains( fileName ) )
				{
					subsLoaded = true;
					fileSubsList += fileName;
				}
			}
			else
			{
				fileSubs.clear();
				subtitlesStream = choosenSubtitlesStream = -1;
				ass->closeASS();
			}

			sPackets.unlock();
		}
	}
	if ( demuxThr )
		demuxThr->emitInfo();
	if ( subsLoaded )
		messageAndOSD( tr( "Załadowane napisy" ) + ": " + Functions::fileName( fileName ), false );
}

void PlayClass::messageAndOSD( const QString &txt, bool onStatusBar, double duration )
{
	if ( ass && QMPlay2Core.getSettings().getBool( "OSD/Enabled" ) )
	{
		osd_mutex.lock();
		ass->getOSD( osd, txt.toUtf8(), duration );
		osd_mutex.unlock();
	}
	if ( onStatusBar )
		emit message( txt, duration * 1000 );
}

void PlayClass::speedMessageAndOSD()
{
	messageAndOSD( tr( "Szybkość odtwarzania" ) + QString( ": %1x" ).arg( speed ) );
	QMPlay2Core.speedChanged( speed );
}

double PlayClass::getARatio()
{
	if ( aRatioName == "auto" )
		return demuxThr->demuxer->streamsInfo()[ videoStream ]->aspect_ratio;
	if ( aRatioName == "sizeDep" )
		return ( double )demuxThr->demuxer->streamsInfo()[ videoStream ]->W / ( double )demuxThr->demuxer->streamsInfo()[ videoStream ]->H;
	return aRatioName.toDouble();
}

void PlayClass::flushAssEvents()
{
	sPackets.lock();
	if ( ass && subtitlesStream > -1 )
		ass->flushASSEvents();
	sPackets.unlock();
}

void PlayClass::stopVThr()
{
	if ( vThr )
	{
		stopVDec();
		if ( vThr )
		{
			vThr->stop();
			vThr = NULL;
		}
		VideoFrame::clearBuffers();
	}
}
void PlayClass::stopAThr()
{
	doSilenceOnStart = false;
	if ( aThr )
	{
		stopADec();
		if ( aThr )
		{
			aThr->stop();
			aThr = NULL;
		}
	}
}

void PlayClass::stopVDec()
{
	if ( vThr && vThr->dec )
	{
		if ( vThr->lock() )
		{
			vThr->destroyDec();
			vThr->destroySubtitlesDecoder();
		}
		else
		{
			vThr->stop( true );
			vThr = NULL;
		}
	}
	frame_last_pts = frame_last_delay = 0.0;
	subtitlesStream = -1;
	nextFrameB = false;
	delete ass; //wywołuje też closeASS(), sMutex nie potrzebny, bo vThr jest zablokowany (mutex przed sMutex)
	ass = NULL;
	fps = 0.0;
}
void PlayClass::stopADec()
{
	if ( aThr && aThr->dec )
	{
		if ( aThr->lock() )
			aThr->destroyDec();
		else
		{
			aThr->stop( true );
			aThr = NULL;
		}
	}
	audio_current_pts = skipAudioFrame = audio_last_delay = 0.0;
}

void PlayClass::setFlip()
{
	if ( vThr )
	{
		if ( vThr->setFlip() )
		{
			QString str;
			switch ( flip )
			{
				case 0:
					str = tr( "Brak odwrócenia obrazu" );
					break;
				case Qt::Horizontal:
					str = tr( "Odbicie lustrzane" );
					break;
				case Qt::Vertical:
					str = tr( "Odbicie pionowe" );
					break;
				case Qt::Horizontal + Qt::Vertical:
					str = tr( "Odwrócenie obrazu" );
					break;
			}
			messageAndOSD( str, true, 0.75 );
		}
		vThr->processParams();
	}
}

void PlayClass::clearPlayInfo()
{
	emit chText( tr( "Zatrzymany" ) );
	emit playStateChanged( false );
	emit updateWindowTitle();
	if ( QMPlay2Core.getSettings().getBool( "ShowCovers" ) )
		emit updateImage();
	emit clearInfo();
}

void PlayClass::saveCover()
{
	if ( demuxThr )
		QMPlay2GUI.saveCover( demuxThr->getCoverFromStream() );
}
void PlayClass::settingsChanged( int page, bool page3Restart )
{
	switch ( page )
	{
		case 1:
			if ( demuxThr )
			{
				if ( !QMPlay2Core.getSettings().getBool( "ShowCovers" ) )
					emit updateImage();
				else
					demuxThr->loadImage();
			}
			break;
		case 2:
			restart();
			break;
		case 3:
			if ( page3Restart )
				restart();
			break;
		case 4: //napisy
			if ( ass && subtitlesStream != -1 )
			{
				ass->setASSStyle();
				if ( vThr )
				{
					vThr->updateSubs();
					vThr->processParams();
				}
			}
			break;
		case 5: //OSD
			if ( ass )
			{
				ass->setOSDStyle();
				osd_mutex.lock();
				if ( osd )
					ass->getOSD( osd, osd->text(), osd->left_duration() );
				osd_mutex.unlock();
			}
			break;
		case 6: //video filters
			if ( vThr )
				vThr->initFilters();
			break;
	}
}
void PlayClass::videoResized( int w, int h )
{
	if ( ass && ( w != videoWinW || h != videoWinH ) )
	{
		ass->setWindowSize( w, h );
		if ( QMPlay2Core.getSettings().getBool( "OSD/Enabled" ) )
		{
			osd_mutex.lock();
			if ( osd )
				ass->getOSD( osd, osd->text(), osd->left_duration() );
			osd_mutex.unlock();
		}
		if ( vThr )
			vThr->updateSubs();
	}
	videoWinW = w;
	videoWinH = h;
}

void PlayClass::setVideoEqualizer( int b, int s, int c, int h )
{
	Brightness = b;
	Saturation = s;
	Contrast = c;
	Hue = h;
	if ( vThr )
	{
		vThr->setVideoEqualizer();
		vThr->processParams();
	}
}

void PlayClass::speedUp()
{
	speed += 0.05;
	if ( speed >= 0.951 && speed <= 1.049 )
		speed = 1.0;
	speedMessageAndOSD();
}
void PlayClass::slowDown()
{
	speed -= 0.05;
	if ( speed >= 0.951 && speed <= 1.049 )
		speed = 1.0;
	else if ( speed < 0.05 )
		speed = 0.05;
	speedMessageAndOSD();
}
void PlayClass::setSpeed()
{
	bool ok;
	double s = QInputDialog::getDouble( NULL, tr( "Szybkość odtwarzania" ), tr( "Ustaw szybkość odtwarzania (sek.)" ), speed, 0.05, 100.0, 2, &ok );
	if ( ok )
	{
		speed = s;
		speedMessageAndOSD();
	}
}
void PlayClass::zoomIn()
{
	if ( vThr )
	{
		zoom += 0.05;
		if ( ass )
			ass->setZoom( zoom );
		messageAndOSD( "Zoom: " + QString::number( zoom ) );
		vThr->setZoom();
		vThr->processParams();
	}
}
void PlayClass::zoomOut()
{
	if ( vThr && zoom - 0.05 > 0.0 )
	{
		zoom -= 0.05;
		if ( ass )
			ass->setZoom( zoom );
		messageAndOSD( "Zoom: " + QString::number( zoom ) );
		vThr->setZoom();
		vThr->processParams();
	}
}
void PlayClass::zoomReset()
{
	if ( zoom != 1. )
	{
		zoom = 1.;
		if ( vThr )
		{
			if ( ass )
				ass->setZoom( zoom );
			messageAndOSD( "Zoom: " + QString::number( zoom ) );
			vThr->setZoom();
			vThr->processParams();
		}
	}
}
void PlayClass::aRatio()
{
	aRatioName = sender()->objectName();
	QString msg_txt = tr( "Współczynnik proporcji" ) + ": " + ( ( QAction * )sender() )->text().remove( '&' );
	if ( vThr && demuxThr && demuxThr->demuxer )
	{
		double aspect_ratio = getARatio();
		if ( ass )
			ass->setARatio( aspect_ratio );
		messageAndOSD( msg_txt );
		vThr->setARatio( aspect_ratio );
		vThr->processParams();
	}
	else
		messageAndOSD( msg_txt );
}
void PlayClass::volume( int v )
{
	vol = v / 100.0;
	if ( !muted )
	{
		emit QMPlay2Core.volumeChanged( vol );
		messageAndOSD( tr( "Głośność" ) + ": " + QString::number( v ) + "%" );
	}
}
void PlayClass::toggleMute()
{
	muted = !muted;
	if ( !muted )
		volume( vol * 100 );
	else
	{
		messageAndOSD( tr( "Dźwięk wyciszony" ) );
		emit QMPlay2Core.volumeChanged( 0.0 );
	}
}
void PlayClass::slowDownVideo()
{
	if ( videoSync <= -maxThreshold )
		return;
	videoSync -= 0.1;
	if ( videoSync < 0.1 && videoSync > -0.1 )
		videoSync = 0.0;
	messageAndOSD( tr( "Opóźnienie obrazu" ) + ": " + QString::number( videoSync ) + "s" );
}
void PlayClass::speedUpVideo()
{
	if ( videoSync >= maxThreshold )
		return;
	videoSync += 0.1;
	if ( videoSync < 0.1 && videoSync > -0.1 )
		videoSync = 0.0;
	messageAndOSD( tr( "Opóźnienie obrazu" ) + ": " + QString::number( videoSync ) + "s" );
}
void PlayClass::setVideoSync()
{
	bool ok;
	double vs = QInputDialog::getDouble( NULL, tr( "Opóźnienie obrazu" ), tr( "Ustaw opóźnienie obrazu (sek.)" ), videoSync, -maxThreshold, maxThreshold, 1, &ok );
	if ( !ok )
		return;
	videoSync = vs;
	messageAndOSD( tr( "Opóźnienie obrazu" ) + ": " + QString::number( videoSync ) + "s" );
}
void PlayClass::slowDownSubs()
{
	if ( subtitlesStream == -1 || ( subtitlesSync - 0.1 < 0 && fileSubs.isEmpty() ) )
		return;
	subtitlesSync -= 0.1;
	if ( subtitlesSync < 0.1 && subtitlesSync > -0.1 )
		subtitlesSync = 0.;
	messageAndOSD( tr( "Opóźnienie napisów" ) + ": " + QString::number( subtitlesSync ) + "s" );
}
void PlayClass::speedUpSubs()
{
	if ( subtitlesStream == -1 )
		return;
	subtitlesSync += 0.1;
	if ( subtitlesSync < 0.1 && subtitlesSync > -0.1 )
		subtitlesSync = 0.;
	messageAndOSD( tr( "Opóźnienie napisów" ) + ": " + QString::number( subtitlesSync ) + "s" );
}
void PlayClass::setSubtitlesSync()
{
	if ( subtitlesStream == -1 )
		return;
	bool ok;
	double ss = QInputDialog::getDouble( NULL, tr( "Opóźnienie napisów" ), tr( "Ustaw opóźnienie napisów (sek.)" ), subtitlesSync, fileSubs.isEmpty() ? 0 : -2147483647, 2147483647, 2, &ok );
	if ( !ok )
		return;
	subtitlesSync = ss;
	messageAndOSD( tr( "Opóźnienie napisów" ) + ": " + QString::number( subtitlesSync ) + "s" );
}
void PlayClass::biggerSubs()
{
	if ( ass )
	{
		if ( subtitlesScale > maxThreshold - 0.05 )
			return;
		subtitlesScale += 0.05;
		ass->setFontScale( subtitlesScale );
		messageAndOSD( tr( "Wielkość napisów" ) + ": " + QString::number( subtitlesScale ) );
		if ( vThr )
		{
			vThr->updateSubs();
			vThr->processParams();
		}
	}
}
void PlayClass::smallerSubs()
{
	if ( ass )
	{
		if ( subtitlesScale <= 0.05 )
			return;
		subtitlesScale -= 0.05;
		ass->setFontScale( subtitlesScale );
		messageAndOSD( tr( "Wielkość napisów" ) + ": " + QString::number( subtitlesScale ) );
		if ( vThr )
		{
			vThr->updateSubs();
			vThr->processParams();
		}
	}
}
void PlayClass::toggleAVS( bool b )
{
	if ( sender()->objectName() == "toggleAudio" )
	{
		audioEnabled = b;
		audioStream = -1;
		if ( !audioEnabled )
			messageAndOSD( tr( "Dźwięk wyłączony" ) );
	}
	else if ( sender()->objectName() == "toggleVideo" )
	{
		videoEnabled = b;
		videoStream = -1;
	}
	else if ( sender()->objectName() == "toggleSubtitles" )
	{
		subtitlesEnabled = b;
		subtitlesStream = -1;
		if ( !subtitlesEnabled )
			messageAndOSD( tr( "Napisy wyłączone" ) );
	}
	reload = true;
}
void PlayClass::setHFlip( bool b )
{
	if ( b )
		flip |= Qt::Horizontal;
	else
		flip &= ~Qt::Horizontal;
	setFlip();
}
void PlayClass::setVFlip( bool b )
{
	if ( b )
		flip |= Qt::Vertical;
	else
		flip &= ~Qt::Vertical;
	setFlip();
}
void PlayClass::screenShot()
{
	if ( vThr )
	{
		vThr->setDoScreenshot();
		emptyBufferCond.wakeAll();
	}
}
void PlayClass::nextFrame()
{
	if ( videoStream > -1 && stopPauseMutex.tryLock() )
	{
		paused = false;
		nextFrameB = fillBufferB = true;
		stopPauseMutex.unlock();
	}
}

void PlayClass::aRatioUpdated( double aRatio ) //jeżeli współczynnik proporcji zmieni się podczas odtwarzania
{
	if ( aRatioName == "auto" && vThr && demuxThr && demuxThr->demuxer && videoStream > -1 )
	{
		demuxThr->demuxer->streamsInfo()[ videoStream ]->aspect_ratio = aRatio;
		double aspect_ratio = getARatio();
		if ( ass )
			ass->setARatio( aspect_ratio );
		vThr->setDeleteOSD();
		vThr->setARatio( aspect_ratio );
		vThr->processParams();
		demuxThr->emitInfo();
	}
}

void PlayClass::demuxThrFinished()
{
	timTerminate.stop();

	if ( !stopPauseMutex.tryLock() )
		doSilenceBreak = true; //jeżeli ta funkcja jest wywołana spod "processEvents()" z "silence()", po tym wywołaniu ma się natychmiast zakończyć

	if ( demuxThr->demuxer ) //Jeżeli wątek się zakończył po upływie czasu timera (nieprawidłowo zakończony), to demuxer nadal istnieje
		demuxThr->end();

	bool br  = demuxThr->demuxer.isAborted(), err = demuxThr->err;
	delete demuxThr;
	demuxThr = NULL;

	stopPauseMutex.unlock();

	if ( restartSeekTo < 0.0 ) //jeżeli nie restart odtwarzania
		fileSubsList.clear(); //czyści listę napisów z pliku
	fileSubs.clear();

	url.clear();
	pos = -1.0;

	emit updateBufferedRange( -1, -1 );
	emit updateLength( 0 );
	emit updatePos( 0 );

	bool clr = true;

	if ( !newUrl.isEmpty() && !quitApp )
	{
		if ( restartSeekTo >= 0.0 ) //jeżeli restart odtwarzania
			stopAVThr();
		emit clearCurrentPlaying();
		play( newUrl );
		newUrl.clear();
		clr = false;
	}
	else
	{
		if ( br || quitApp )
			emit clearCurrentPlaying();

		if ( !br && !quitApp )
		{
			if ( err && !ignorePlaybackError ) //Jeżeli wystąpił błąd i nie jest on ignorowany
				stopAVThr();
			else
			{
				emit playNext( err );
				clr = false;
			}
			emit clearCurrentPlaying();
		}
		else
			stopAVThr();
	}

	if ( clr )
		clearPlayInfo();
	else
		emit updateBuffered( -1, -1, 0.0, 0.0 );

	if ( quitApp )
	{
		qApp->processEvents();
		emit quit();
	}
}

void PlayClass::timTerminateFinished()
{
	timTerminate.stop();
	if ( demuxThr && demuxThr->isRunning() )
		demuxThr->terminate();
	emit QMPlay2Core.restoreCursor();
}

static Decoder *loadStream( const QList< StreamInfo * > &streams, const int choosenStream, int &stream, const QMPlay2MediaType type, const QString &lang, Writer *writer = NULL )
{
	Decoder *dec = NULL;
	const bool subtitles = type == QMPLAY2_TYPE_SUBTITLE;
	if ( choosenStream >= 0 && choosenStream < streams.count() && streams[ choosenStream ]->type == type )
	{
		if ( streams[ choosenStream ]->must_decode || !subtitles )
			dec = Decoder::create( streams[ choosenStream ], writer, QMPlay2GUI.getModules( "decoders", 7 ) );
		if ( dec || subtitles )
			stream = choosenStream;
	}
	else
	{
		int defaultStream = -1, choosenLangStream = -1;
		for ( int i = 0 ; i < streams.count() ; ++i )
		{
			if ( streams[ i ]->type == type )
			{
				if ( defaultStream < 0 && streams[ i ]->is_default )
					defaultStream = i;
				if ( !lang.isEmpty() && choosenLangStream < 0 )
				{
					foreach ( const QMPlay2Tag &tag, streams[ i ]->other_info )
					{
						if ( tag.first.toInt() == QMPLAY2_TAG_LANGUAGE )
						{
							if ( tag.second == lang )
								choosenLangStream = i;
							break;
						}
					}
				}
			}
		}
		if ( choosenLangStream > -1 )
			defaultStream = choosenLangStream;
		for ( int i = 0 ; i < streams.count() ; ++i )
		{
			StreamInfo *streamInfo = streams[ i ];
			if ( streamInfo->type == type && ( defaultStream == -1 || i == defaultStream ) )
			{
				if ( streamInfo->must_decode || !subtitles )
					dec = Decoder::create( streamInfo, writer, QMPlay2GUI.getModules( "decoders", 7 ) );
				if ( dec || subtitles )
				{
					stream = i;
					break;
				}
			}
		}
	}
	return dec;
}
void PlayClass::load( Demuxer *demuxer )
{
	QList< StreamInfo * > streams = demuxer->streamsInfo();
	Decoder *dec = NULL;

	if ( videoStream < 0 || ( choosenVideoStream > -1 && choosenVideoStream != videoStream ) ) //load video
	{
		vPackets.clear();
		stopVDec(); //lock
		if ( videoEnabled )
			dec = loadStream( streams, choosenVideoStream, videoStream, QMPLAY2_TYPE_VIDEO, QString(), vThr ? vThr->getHWAccelWriter() : NULL );
		else
			dec = NULL;
		if ( dec )
		{
			const bool canEmitVideoStarted = !vThr;
			if ( vThr && ( vThr->getHWAccelWriter() != dec->HWAccel() ) )
				stopVThr();
			if ( !vThr )
			{
				vThr = new VideoThr( *this, dec->HWAccel(), QMPlay2GUI.getModules( "videoWriters", 5 ) );
				vThr->setSyncVtoA( QMPlay2Core.getSettings().getBool( "SyncVtoA" ) );
			}
			if ( vThr->isRunning() )
			{
				vThr->setFrameSize( streams[ videoStream ]->W, streams[ videoStream ]->H );
				vThr->setARatio( getARatio() );
				vThr->setVideoEqualizer();
				vThr->setDec( dec );
				vThr->setZoom();
				vThr->setFlip();
				vThr->initFilters( false );

				if ( !vThr->processParams() )
					dec = NULL;
				else
				{
					fps = streams[ videoStream ]->FPS;
					ass = new LibASS( QMPlay2Core.getSettings() );
					ass->setWindowSize( videoWinW, videoWinH );
					ass->setFontScale( subtitlesScale );
					ass->setARatio( getARatio() );
					ass->setZoom( zoom );

#if defined Q_OS_WIN && !defined Q_OS_WIN64
					if ( LibASS::slowFontCacheUpdate() && firsttimeUpdateCache )
					{
						UpdateFC updateFC( ass );
						firsttimeUpdateCache = false;
					}
					else
#endif
						ass->initOSD();

					if ( canEmitVideoStarted )
						emit videoStarted();

					if ( reload )
						seekTo = SEEK_STREAM_RELOAD;
				}
				vThr->unlock();
			}
			else
			{
				delete dec;
				dec = NULL;
			}
		}
		if ( !dec )
		{
			choosenVideoStream = videoStream = -1;
			stopVThr();
		}
	}

	if ( audioStream < 0 || ( choosenAudioStream > -1 && choosenAudioStream != audioStream ) ) //load audio
	{
		aPackets.clear();
		stopADec(); //lock
		if ( audioEnabled )
			dec = loadStream( streams, choosenAudioStream, audioStream, QMPLAY2_TYPE_AUDIO, choosenAudioLang );
		else
			dec = NULL;
		if ( dec )
		{
			if ( !aThr )
				aThr = new AudioThr( *this, QMPlay2GUI.getModules( "audioWriters", 5 ) );
			if ( aThr->isRunning() )
			{
				aThr->setDec( dec );
				quint32 srate = 0;
				quint8 chn = 0;
				if ( QMPlay2Core.getSettings().getBool( "ForceSamplerate" ) )
					srate = QMPlay2Core.getSettings().getUInt( "Samplerate" );
				if ( QMPlay2Core.getSettings().getBool( "ForceChannels" ) )
					chn = QMPlay2Core.getSettings().getUInt( "Channels" );

				if ( !aThr->setParams( streams[ audioStream ]->channels, streams[ audioStream ]->sample_rate, chn, srate ) )
					dec = NULL;
				else if ( reload )
					seekTo = SEEK_STREAM_RELOAD;
				if ( doSilenceOnStart )
				{
					aThr->silence( true );
					doSilenceOnStart = false;
				}
				aThr->unlock();
			}
			else
			{
				delete dec;
				dec = NULL;
			}
		}
		if ( !dec )
		{
			choosenAudioStream = audioStream = -1;
			stopAThr();
		}
	}

	//load subtitles
	if ( subtitlesStream == -1 || ( choosenSubtitlesStream > -1 && choosenSubtitlesStream != subtitlesStream ) )
	{
		if ( !QMPlay2Core.getSettings().getBool( "KeepSubtitlesDelay" ) )
			subtitlesSync = 0.0;
		fileSubs.clear();
		clearSubtitlesBuffer();
		if ( videoStream >= 0 && vThr )
		{
			sPackets.lock();
			vThr->destroySubtitlesDecoder();
			ass->closeASS();
			ass->clearFonts();
			sPackets.unlock();

			if ( subtitlesEnabled && fileSubsList.count() && choosenSubtitlesStream < 0 )
				loadSubsFile( fileSubsList[ fileSubsList.count() - 1 ] );
			else
			{
				if ( subtitlesEnabled )
					dec = loadStream( streams, choosenSubtitlesStream, subtitlesStream, QMPLAY2_TYPE_SUBTITLE, choosenSubtitlesLang );
				else
				{
					subtitlesStream = -1;
					dec = NULL;
				}
				if ( vThr && subtitlesStream > -1 )
				{
					if ( subtitlesSync < 0.0 )
						subtitlesSync = 0.0;
					sPackets.lock();
					if ( dec )
						vThr->setSubtitlesDecoder( dec );
					QByteArray assHeader = streams[ subtitlesStream ]->data;
					if ( !assHeader.isEmpty() && ( streams[ subtitlesStream ]->codec_name == "ssa" || streams[ subtitlesStream ]->codec_name == "ass" ) )
					{
						for ( int i = 0 ; i < streams.count() ; ++i )
							if ( streams[ i ]->type == QMPLAY2_TYPE_ATTACHMENT && ( streams[ i ]->codec_name == "TTF" || streams[ i ]->codec_name == "OTF" ) && streams[ i ]->data.size() )
								ass->addFont( streams[ i ]->title, streams[ i ]->data );
					}
					else
						assHeader.clear();
					ass->initASS( assHeader );
					if ( reload )
						seekTo = SEEK_STREAM_RELOAD;
					sPackets.unlock();
				}
				else
				{
					subtitlesStream = choosenSubtitlesStream = -1;
					if ( dec )
					{
						delete dec;
						dec = NULL;
					}
				}
			}
		}
	}

	reload = false;
	loadMutex.unlock();
}
