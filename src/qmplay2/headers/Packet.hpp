#ifndef PACKET_HPP
#define PACKET_HPP

#include <TimeStamp.hpp>

#include <QByteArray>

struct Packet : public QByteArray
{
	inline Packet() :
		sampleAspectRatio( 0.0 ),
		hasKeyFrame( true )
	{}

	inline void reset()
	{
		*this = Packet();
	}

	TimeStamp ts;
	double duration, sampleAspectRatio;
	bool hasKeyFrame;
};

#endif
