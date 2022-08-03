#ifndef _ABXFUNCTIONS_HPP
#define _ABXFUNCTIONS_HPP

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <vector>
#include <algorithm>
#include <openssl/evp.h>

// ABX (Android Binary XML) to XML converter
// Copyright (c) 2022, _that
// SPDX-License-Identifier: GPL-3.0-only

// test: g++ -g abxtoxml.cpp `pkg-config --libs openssl` && ./a.out packages.xml

// based on https://www.cclsolutionsgroup.com/post/android-abx-binary-xml

using namespace std;

class AbxToXml
{
	std::istream& m_is;
	std::ostream& m_os;
	bool mTagOpen = false;
	std::vector<std::string> mInternedStrings;
	bool mError = true;

public:
	AbxToXml(std::istream& is, std::ostream& os) : m_is(is), m_os(os) {}

	bool run()
	{
		mError = true;
		char buffer[4];
		m_is.read(buffer, 4);
		if (memcmp(buffer, "ABX", 3) != 0)
			// TODO: handle error: not an ABX file
			return false;

		if (buffer[3] != 0)
			// TODO: handle error: we only understand ABX version 0
			return false;

		mError = false;
		return convert_abx_content();
	}

private:
	enum Event
	{
		// based on XmlPullParser.java in libcore
		START_DOCUMENT = 0,
		END_DOCUMENT = 1,
		START_TAG = 2,
		END_TAG = 3,
		TEXT = 4,
		CDSECT = 5,
		ENTITY_REF = 6,
		IGNORABLE_WHITESPACE = 7,
		PROCESSING_INSTRUCTION = 8,
		COMMENT = 9,
		DOCDECL = 10,

		// based on BinaryXmlSerializer.java
		ATTRIBUTE = 15,
	};

	// based on BinaryXmlSerializer.java
	enum Type
	{
		TYPE_NULL = 1 << 4,
		TYPE_STRING = 2 << 4,
		TYPE_STRING_INTERNED = 3 << 4,
		TYPE_BYTES_HEX = 4 << 4,
		TYPE_BYTES_BASE64 = 5 << 4,
		TYPE_INT = 6 << 4,
		TYPE_INT_HEX = 7 << 4,
		TYPE_LONG = 8 << 4,
		TYPE_LONG_HEX = 9 << 4,
		TYPE_FLOAT = 10 << 4,
		TYPE_DOUBLE = 11 << 4,
		TYPE_BOOLEAN_TRUE = 12 << 4,
		TYPE_BOOLEAN_FALSE = 13 << 4,
	};


	bool convert_abx_content()
	{
		char b;
		while (m_is.get(b))
		{
			Event ev = Event(b & 0xf);
			Type ty = Type(b & 0xf0);

			if (ev == ATTRIBUTE)
			{
				// TODO: verify that tag is still open
				m_os << " " << read_data(TYPE_STRING_INTERNED);
				m_os << "=\"";
				m_os << read_data(ty);		// TODO: escaping?
				m_os << '"';
				continue;
			}
			if (mTagOpen)
			{
				m_os << ">";
				mTagOpen = false;
			}
			std::string data = read_data(ty);
			dispatch_event(ev, data);
		}
		return !mError;
	}

	void dispatch_event(Event ev, const std::string& data)
	{
		switch (ev)
		{
			case START_DOCUMENT:
			case END_DOCUMENT:
				// TODO: track that we actually started the document
				break;

			case START_TAG:
				m_os << "<" << data;
				mTagOpen = true;
				break;

			case END_TAG:
				m_os << "</" << data << ">";
				break;

			case TEXT:
				m_os << data;
				break;

			case CDSECT:
				m_os << "<![CDATA[" <<  data << "]]>";
				break;

			case ENTITY_REF:
			case IGNORABLE_WHITESPACE:
			case PROCESSING_INSTRUCTION:
			case COMMENT:
			case DOCDECL:
				m_os << data;
				// TODO
				break;

			default:
				m_os << "#error: Invalid event " << int(ev);
				mError = true;
				break;
		}
	}

	template <typename T> static T read_bswap(std::istream& is)
	{
		char buffer[sizeof(T)];
		is.read(buffer, sizeof(T));
		std::reverse(std::begin(buffer), std::end(buffer));
		return *reinterpret_cast<T*>(buffer);
	}

	static uint16_t read_uint16(std::istream& is)
	{
		return read_bswap<uint16_t>(is);
	}

	static std::string read_string(std::istream& is)
	{
		uint16_t length = read_uint16(is);
		std::string s;
		s.resize(length);
		is.read(&s[0], length);
		return s;
	}


	std::string read_data(Type ty)
	{
		switch (ty)
		{
			case TYPE_NULL:
				return {};

			case TYPE_STRING:
				return read_string(m_is);

			case TYPE_STRING_INTERNED:
				{
					uint16_t id = read_uint16(m_is);
					if (id == 0xffff)
					{
						std::string s = read_string(m_is);
						mInternedStrings.push_back(s);
						return s;
					}
					if (id >= mInternedStrings.size())
					{
						// TODO: handle error
						mError = true;
						return "#error: invalid string ID";
					}
					return mInternedStrings.at(id);
				}

			case TYPE_BYTES_HEX:
				{
					std::string s = read_string(m_is);
					std::string hex;
					static const char* hexdigits = "0123456789abcdef";
					for (unsigned char c : s)
					{
						hex += hexdigits[c >> 4];
						hex += hexdigits[c & 0xf];
					}
					return hex;
				}

			case TYPE_BYTES_BASE64:
				{
					std::string s = read_string(m_is);
					std::string b64;
					auto outlen = ((s.length() + 2) / 3) * 4;
					b64.resize(outlen+1);		// +1 for null terminator
					auto got = EVP_EncodeBlock(
						reinterpret_cast<unsigned char *>(&b64[0]),
						reinterpret_cast<const unsigned char*>(s.c_str()), s.length());
					if (got != outlen)
					{
						mError = true;
						return "#error: base64 encoding failed";  // TODO
					}
					b64.resize(outlen);
					return b64;
				}

			case TYPE_INT_HEX:
				// TODO: output hex instead of dec (what is the exact format?)
			case TYPE_INT:
				{
					int32_t val = read_bswap<int>(m_is);
					std::stringstream ss;
					ss << val;
					return ss.str();
				}

			case TYPE_LONG_HEX:
				// TODO: output hex instead of dec (what is the exact format?)
			case TYPE_LONG:
				{
					int64_t val = read_bswap<long>(m_is);
					std::stringstream ss;
					ss << val;
					return ss.str();
				}

			case TYPE_FLOAT:
				{
					float val = read_bswap<float>(m_is);
					std::stringstream ss;
					ss << val;
					return ss.str();
				}

			case TYPE_DOUBLE:
				{
					double val = read_bswap<double>(m_is);
					std::stringstream ss;
					ss << val;
					return ss.str();
				}

			case TYPE_BOOLEAN_TRUE:
				return "1";

			case TYPE_BOOLEAN_FALSE:
				return "0";
		}
		// TODO
		mError = true;
		return "#error: invalid type";
	}
};
#endif // _ABXFUNCTIONS_HPP
