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
#include <ctype.h>
#include <string.h>

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
        return "SV";
    }

    return "invalid";
}

static void
string_latin1_to_ascii(std::string &s)
{
    for (std::string::size_type i = 0; i < s.length(); ++i) {
        if ((s[i] & 0x80) == 0)
            continue;

        switch (s[i]) {
        case '\xc4':
            s.replace(i, 1, "AE");
            ++i;
            break;

        case '\xd6':
            s.replace(i, 1, "OE");
            ++i;
            break;

        case '\xdc':
            s.replace(i, 1, "UE");
            ++i;
            break;

        case '\xdf':
            s.replace(i, 1, "ss");
            ++i;
            break;

        case '\xe4':
            s.replace(i, 1, "ae");
            ++i;
            break;

        case '\xf6':
            s.replace(i, 1, "oe");
            ++i;
            break;

        case '\xfc':
            s.replace(i, 1, "ue");
            ++i;
            break;

        default:
            s[i] = ' ';
        }
    }
}

static void
string_to_upper(std::string &s)
{
    for (std::string::size_type i = 0; i < s.length(); ++i)
        s[i] = toupper(s[i]);
}

static void
pipe_split(std::string &a, std::string &b)
{
    std::string::size_type pos;

    pos = a.find('|');
    if (pos == std::string::npos)
        return;

    b = std::string(a, pos + 1);
    a = std::string(a, 0, pos);
}

void
CenfisAirspaceWriter::write(const Airspace &as)
{
    /* ignore some airspace types */
    if (as.getType() == Airspace::TYPE_UNKNOWN ||
        as.getType() == Airspace::TYPE_ECHO_LOW ||
        as.getType() == Airspace::TYPE_ECHO_HIGH ||
        as.getType() == Airspace::TYPE_GLIDER)
        return;

    CenfisBuffer current;
    current.make_header();

    std::string name = as.getName(), name2, name3, name4, type_string;
    string_latin1_to_ascii(name);
    string_to_upper(name);
    pipe_split(name, name2);

    if (name2.length() > 0) {
        /* hacked name from CenfisTextAirspaceReader */
        pipe_split(name2, name3);
        pipe_split(name3, name4);
        pipe_split(name4, type_string);
    } else {
        if (as.getType() == Airspace::TYPE_RESTRICTED &&
            name.compare(0, 3, "EDR") == 0) {
            name.erase(0, 3);
            name4 = "ED";
        } else if ((as.getType() == Airspace::TYPE_RESTRICTED &&
                    name.compare(0, 4, "ED-R") == 0) ||
                   (as.getType() == Airspace::TYPE_DANGER &&
                    name.compare(0, 4, "ED-D") == 0)) {
            name.erase(0, 4);
            name4 = "ED";
        }

        if (name.compare(0, 2, "HX") == 0) {
            name.erase(0, 2);
            name3 = "HX";
        } else if (name.length() > 5 &&
                   name.compare(name.length() - 5, 5, " (HX)") == 0) {
            name.erase(name.length() - 5, 5);
            name3 = "HX";
        }

        if (name.length() > 6 &&
            name.compare(name.length() - 6, 6, " (TRA)") == 0) {
            name.erase(name.length() - 6, 6);
            name2 = "TRA";
        }
    }

    /* AC = type */

    bool has_first = true;

    current.header().ac_rel_ind = htons(current.tell());
    if (type_string.length() > 0) {
        if (type_string[0] == '_') {
            /* reproduce bug: there is no first vertex, marked with an
               underscore by CenfisTextAirspaceReader */
            has_first = false;
            type_string.erase(type_string.begin());
        }

        current.append(type_string);
    } else
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
    current.append(name);

    if (name2.length() > 0 && name2[0] != '-') {
        current.header().an2_rel_ind = htons(current.tell());
        current.append(name2);
    }

    if (name3.length() > 0) {
        current.header().an3_rel_ind = htons(current.tell());
        current.append(name3);
    }

    if (name4.length() > 0) {
        current.header().an4_rel_ind = htons(current.tell());
        current.append(name4);
    }

    if (name2.length() > 0 && name2[0] == '-') {
        /* reproduce bug: AN4 before AN2, marked with a dash by
           CenfisTextAirspaceReader */
        current.header().an2_rel_ind = htons(current.tell());
        name2.erase(name2.begin());
        current.append(name2);
    }

    /* AL = lower bound */

    if (as.getBottom().defined() &&
        (as.getBottom().getRef() != Altitude::REF_GND ||
         as.getBottom().getValue() != 0)) {
        current.header().al_rel_ind = htons(current.tell());
        current.append(as.getBottom());
    }

    /* AH = upper bound */

    if (as.getTop2().defined())
        current.append(as.getTop2());

    if (as.getTop().defined()) {
        current.header().ah_rel_ind = htons(current.tell());
        current.append(as.getTop());
    }

    /* FIS = frequency */

    if (as.getFrequency().defined()) {
        current.header().fis_rel_ind = htons(current.tell());
        current.append(as.getFrequency());
    }

    /* S, L = vertices */

    const Airspace::EdgeList &edges = as.getEdges();
    /* bug reproduction: if an airspace has no first vertex, it
       inherits the first vertex of the previous one */
    static SurfacePosition buffer;
    const SurfacePosition *firstVertex = has_first ? NULL : &buffer;
    size_t l_size_offset = 0;

    for (Airspace::EdgeList::const_iterator it = edges.begin();
         it != edges.end(); ++it) {
        const Edge &edge = *it;
        if (firstVertex != NULL) {
            current.append(edge, *firstVertex);
        } else if (edge.getType() == Edge::TYPE_CIRCLE) {
            current.header().c_rel_ind = htons(current.tell());
            current.append(edge, *firstVertex);
        } else if (edge.getType() == Edge::TYPE_VERTEX) {
            current.header().s_rel_ind = htons(current.tell());
            buffer = edge.getEnd();
            firstVertex = &buffer;
            current.append_first(*firstVertex);
            current.header().l_rel_ind = htons(current.tell());

            l_size_offset = current.tell();
            current.append_byte(0xff);
        } else {
            // XXX there was no first vertex?
        }
    }

    if (l_size_offset > 0)
        current[l_size_offset] = current.tell() - l_size_offset - 1;

    /* ap_rel_ind (anchor point) */

    if (firstVertex != NULL) {
        current.header().ap_rel_ind = htons(current.tell());
        current.append_anchor(*firstVertex);
    }

    current.header().asp_rec_lengh = htons(current.tell());

    current.header().voice_ind = htons(as.getVoice());

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

    header.asp_index.offset = htonl(0x60000 + offset);
    header.asp_index.total_size = htons(index_buffer.tell());
    header.asp_index.num_elements = htons(index_buffer.tell() / 2);
    offset += index_buffer.tell();

    header.config.offset = htonl(0x60000 + offset);
    header.config.total_size = htons(config_buffer.tell());
    header.config.num_elements = htons(config_buffer.tell() / 4);

    /* pad to next 0x10 */
    config_buffer.fill(0x00, (-(offset + config_buffer.tell())) & 0xf);

    if (offset + config_buffer.tell() > 0x10000)
        throw container_full("the Cenfis has only 0x10000 bytes airspace buffer");

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
