/*
 * loggertools
 * Copyright (C) 2004-2007 Max Kellermann <max@duempel.org>
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
#include "cenfis-airspace.h"
#include "cenfis-buffer.hh"

#include <ostream>
#include <iomanip>

#include <netinet/in.h>

class CenfisAirspaceWriter : public AirspaceWriter {
public:
    std::ostream *stream;
    bool first;
    CenfisBuffer airspace_buffer, index_buffer, config_buffer;

public:
    CenfisAirspaceWriter(std::ostream *stream);

private:
    void write(const CenfisBuffer &src);

public:
    virtual void write(const Airspace &as);
    virtual void flush();
};

CenfisAirspaceWriter::CenfisAirspaceWriter(std::ostream *_stream)
    :stream(_stream), first(true),
     airspace_buffer(sizeof(struct cenfis_airspace_file_header)),
     index_buffer() {
}

void
CenfisAirspaceWriter::write(const CenfisBuffer &src)
{
    std::ostream::pos_type length = (std::ostream::pos_type)src.tell();
    std::ostream::pos_type pos = stream->tellp();
    if (pos / 0x8000 != (pos + length) / 0x8000)
        /* don't write across bank limit, insert 0xff padding
           instead */
        for (std::streamsize padding = ((pos + length) / 0x8000) * 0x8000 - pos;
             padding > 0; --padding)
            stream->put((std::ostream::char_type)0xff);

    *stream << src;
}

static const char *
AirspaceTypeToString(Airspace::type_t type)
{
    switch (type) {
    case Airspace::TYPE_UNKNOWN:
        return "unknown";

    case Airspace::TYPE_ALPHA:
        return "A";

    case Airspace::TYPE_BRAVO:
        return "B";

    case Airspace::TYPE_CHARLY:
        return "C";

    case Airspace::TYPE_DELTA:
        return "D";

    case Airspace::TYPE_ECHO_LOW:
    case Airspace::TYPE_ECHO_HIGH:
        return "E";

    case Airspace::TYPE_FOX:
        return "F";

    case Airspace::TYPE_CTR:
        return "CTR";

    case Airspace::TYPE_TMZ:
        return "TMZ";

    case Airspace::TYPE_RESTRICTED:
        return "R";

    case Airspace::TYPE_DANGER:
        return "D";

    case Airspace::TYPE_GLIDER:
        return "glider";
    }

    return "invalid";
}

void
CenfisAirspaceWriter::write(const Airspace &as)
{
    CenfisBuffer current;
    current.make_header();

    /* AC = type */

    current.header().ac_rel_ind = htons(current.tell());
    current.append(AirspaceTypeToString(as.getType()));

    /* file info */

    if (first) {
        // XXX
        current.header().file_info_ind = htons(current.tell());
        current.append("ASP_X304.BHF29-7-2007   ");
        first = false;
    }

    /* AN = name */

    current.header().an_rel_ind = htons(current.tell());
    current.append(as.getName());

    // XXX AN3?

    current.header().an3_rel_ind = htons(current.tell());
    current.append("A");

    /* AL = lower bound */

    if (as.getBottom().defined() &&
        (as.getBottom().getRef() != Altitude::REF_GND ||
         as.getBottom().getValue() != 0)) {
        current.header().al_rel_ind = htons(current.tell());
        current.append(as.getBottom());
    }

    /* AH = upper bound */

    if (as.getTop().defined()) {
        current.header().ah_rel_ind = htons(current.tell());
        current.append(as.getTop());
    }

    /* S, L = vertices */

    unsigned num_vertices = 0;
    Angle::value_t latitude_sum = 0, longitude_sum = 0;

    const Airspace::VertexList &vertices = as.getVertices();
    const SurfacePosition *firstVertex = NULL;
    for (Airspace::VertexList::const_iterator it = vertices.begin();
         it != vertices.end(); ++it) {
        if (firstVertex == NULL) {
            current.header().s_rel_ind = htons(current.tell());
            current.append(*it);
            firstVertex = &(*it);
            current.header().l_rel_ind = htons(current.tell());
            current.append_byte((vertices.size() - 1) * 4);
        } else {
            current.append(*it, *firstVertex);
        }

        latitude_sum += (*it).getLatitude().refactor(60);
        longitude_sum += (*it).getLongitude().refactor(60);
        ++num_vertices;
    }

    /* ap_rel_ind (anchor point) */

    if (firstVertex != NULL) {
        current.header().ap_rel_ind = htons(current.tell());
        current.append_byte(4);
        current.append_short(latitude_sum / num_vertices - firstVertex->getLatitude().refactor(60));
        current.append_short(longitude_sum / num_vertices - firstVertex->getLongitude().refactor(60));
    }

    current.header().asp_rec_lengh = htons(current.tell());

    index_buffer.append_short(sizeof(struct cenfis_airspace_file_header) + airspace_buffer.tell());
    airspace_buffer << current;
}

void
CenfisAirspaceWriter::flush()
{
    if (stream == NULL)
        throw already_flushed();

    config_buffer.append_byte(0x00);
    config_buffer.fill(0x01, 0xe1);
    config_buffer.encrypt(0xe2);

    struct cenfis_airspace_file_header header;
    size_t offset = sizeof(header);

    memset(&header, 0xff, sizeof(header));
    header.asp.offset = htonl(0x60000 + offset);
    header.asp.total_size = htons(airspace_buffer.tell());
    header.asp.num_elements = htons(index_buffer.tell() / 2);
    offset += airspace_buffer.tell();

    if (offset < 0x8000) {
        airspace_buffer.fill(0xff, 0x8000 - offset);
        offset = 0x8000;
    }

    header.asp_index.offset = htonl(offset);
    header.asp_index.total_size = htons(index_buffer.tell());
    header.asp_index.num_elements = htons(index_buffer.tell() / 2);
    offset += index_buffer.tell();

    header.config.offset = htonl(offset);
    header.config.total_size = htons(config_buffer.tell());
    header.config.num_elements = htons(config_buffer.tell() / 4);

    stream->write((const std::ostream::char_type*)&header, sizeof(header));
    *stream << airspace_buffer << index_buffer << config_buffer;

    stream = NULL;
}

AirspaceReader *
CenfisAirspaceFormat::createReader(std::istream *) const
{
    return NULL;
}

AirspaceWriter *
CenfisAirspaceFormat::createWriter(std::ostream *stream) const
{
    return new CenfisAirspaceWriter(stream);
}