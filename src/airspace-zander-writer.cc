/*
 * loggertools
 * Copyright (C) 2004-2008 Max Kellermann <max@duempel.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the License
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "exception.hh"
#include "airspace.hh"
#include "airspace-io.hh"

#include <ostream>
#include <iomanip>

#include <stdlib.h>

class ZanderAirspaceWriter : public AirspaceWriter {
public:
    std::ostream *stream;
public:
    ZanderAirspaceWriter(std::ostream *stream);

public:
    virtual void write(const Airspace &as);
    virtual void flush();
};

ZanderAirspaceWriter::ZanderAirspaceWriter(std::ostream *_stream)
    :stream(_stream) {
    *stream << "* Written by loggertools\n\n";
}

static const char *type_to_string(Airspace::type_t type) {
    switch (type) {
    case Airspace::TYPE_UNKNOWN:
        return "UNKNOWN";
    case Airspace::TYPE_ALPHA:
        return "A";
    case Airspace::TYPE_BRAVO:
        return "B";
    case Airspace::TYPE_CHARLY:
        return "C";
    case Airspace::TYPE_DELTA:
        return "D";
    case Airspace::TYPE_ECHO_LOW:
        return "E";
    case Airspace::TYPE_ECHO_HIGH:
        return "W";
    case Airspace::TYPE_FOX:
        return "F";
    case Airspace::TYPE_CTR:
        return "CTR";
    case Airspace::TYPE_TMZ:
        return "TMZ";
    case Airspace::TYPE_RESTRICTED:
        return "R";
    case Airspace::TYPE_DANGER:
        return "Q";
    case Airspace::TYPE_GLIDER:
        return "GSEC";
    }

    return "INVALID";
}

static std::ostream &operator <<(std::ostream &os, Airspace::type_t type) {
    return os << type_to_string(type);
}

static const char *altitude_ref_to_string(Altitude::ref_t ref) {
    switch (ref) {
    case Altitude::REF_UNKNOWN:
        return "UNKNOWN";
    case Altitude::REF_MSL:
        return "MSL";
    case Altitude::REF_GND:
    case Altitude::REF_AIRFIELD:
        return "GND";
    case Altitude::REF_1013:
        return "FL";
    }

    return "INVALID";
}

static std::ostream &operator <<(std::ostream &os, Altitude::ref_t ref) {
    return os << altitude_ref_to_string(ref);
}

static std::ostream &operator <<(std::ostream &os, const Altitude &alt) {
    if (!alt.defined())
        return os << "UNKNOWN";

    return os << std::setfill('0') << std::setw(5) << alt.getValue()
              << " " << alt.getRef();
}

static std::ostream &
operator <<(std::ostream &os, const Distance &distance)
{
    if (!distance.defined())
        return os << "UNKNOWN";

    return os << std::setfill('0') << std::fixed
              << std::setw(7) << std::setprecision(3)
              << distance.toUnit(Distance::UNIT_NAUTICAL_MILES).getValue();
}

static std::ostream &
operator <<(std::ostream &os, const SurfacePosition &position)
{
    int latitude = position.getLatitude().refactor(60);
    int absLatitude = abs(latitude);
    int longitude = position.getLongitude().refactor(60);
    int absLongitude = abs(longitude);

    return os << std::setfill('0') << std::setw(2) << (absLatitude / 3600)
              << std::setfill('0') << std::setw(2) << ((absLatitude / 60) % 60)
              << std::setfill('0') << std::setw(2) << (absLatitude % 60)
              << (latitude < 0 ? 'S' : 'N') << ' '
              << std::setfill('0') << std::setw(3) << (absLongitude / 3600)
              << std::setfill('0') << std::setw(2) << ((absLongitude / 60) % 60)
              << std::setfill('0') << std::setw(2) << (absLongitude % 60)
              << (longitude < 0 ? 'W' : 'E');
}

static void
write_vertex(std::ostream &stream, const Edge &edge, char symbol)
{
    stream << symbol << ' ' << edge.getEnd() << "\n";
}

static void
write_circle(std::ostream &stream, const Edge &edge)
{
    stream << "C " << edge.getCenter() << "\n"
           << "  +" << edge.getRadius() << "\n";
}

static void
write_arc(std::ostream &stream, const Edge &edge,
          const Edge &prev)
{
    stream << "A " << edge.getCenter() << "\n"
           << "  " << prev.getEnd() << "\n"
           << (edge.getSign() < 0 ? '-' : '+')
           << "\n";
}

void ZanderAirspaceWriter::write(const Airspace &as) {
    std::string name = as.getName();
    if (name.length() > 10)
        name.erase(10);

    *stream << "N "
            << std::setfill(' ') << std::setw(10) << std::left << name
            << std::right
            << " " << as.getType() << "\n"
            << "  " << as.getTop() << "\n"
            << "  " << as.getBottom() << "\n";

    char vertex_symbol = 'S';
    const Airspace::EdgeList &edges = as.getEdges();
    for (Airspace::EdgeList::const_iterator it = edges.begin();
         it != edges.end(); ++it) {
        const Edge &edge = (*it);
        switch (edge.getType()) {
        case Edge::TYPE_VERTEX:
            write_vertex(*stream, edge, vertex_symbol);
            vertex_symbol = 'L';
            break;

        case Edge::TYPE_CIRCLE:
            write_circle(*stream, edge);
            break;

        case Edge::TYPE_ARC:
            if (it != edges.begin()) {
                Airspace::EdgeList::const_iterator prev = it;
                --prev;
                if (prev->getType() == Edge::TYPE_VERTEX)
                    write_arc(*stream, edge, *prev);
            }
            break;
        }
    }

    if (!edges.empty() && edges.begin()->getType() == Edge::TYPE_VERTEX)
        write_vertex(*stream, *edges.begin(), vertex_symbol);

    *stream << "\n";
}

void ZanderAirspaceWriter::flush()
{
    if (stream == NULL)
        throw already_flushed();

    stream = NULL;
}

AirspaceReader *
ZanderAirspaceFormat::createReader(std::istream *) const
{
    return NULL;
}

AirspaceWriter *
ZanderAirspaceFormat::createWriter(std::ostream *stream) const
{
    return new ZanderAirspaceWriter(stream);
}
