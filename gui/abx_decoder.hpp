/*
	Copyright 2022 frknkrc44/Furkan Karcioglu
	This file is part of TWRP/TeamWin Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <map>
#include <list>
#include <iostream>
#include <cstring>
#include <vector>

class XMLAttribute {
    public:
        std::vector<char> mValue;
        int mDataType;
        XMLAttribute(int dataType, std::vector<char> value) {
            mDataType = dataType;
            mValue.insert(mValue.begin(), value.begin(), value.end());
        }
};

class XMLElement {
    public:
        std::vector<char> mTagName;
        std::vector<char> text;
        std::map<std::vector<char>, XMLAttribute*>* attributes;
        std::list<XMLElement*> subElements;

        XMLElement() {
            attributes = new std::map<std::vector<char>, XMLAttribute*>();
        }

        XMLElement(std::vector<char> tagName) {
            attributes = new std::map<std::vector<char>, XMLAttribute*>();
            mTagName.clear();
            mTagName.insert(mTagName.begin(), tagName.begin(), tagName.end());
        }
};

class AbxDecoder {
    public:
        int isAbx() {
            // maybe empty?
            if(mInput.size() < 5) 
                return 0;

            curPos = 0;
            std::vector<char> headerV = readFromCurPos(4);
            const char* header = reinterpret_cast<const char*>(headerV.data());
            return strcmp(header, startMagic) == 0;
        }

        std::string* getDecodedOutput() {
            if(isAbx())
                std::cerr << "ABX File Found" << std::endl;
            else
                return NULL;
            
            internedStrings.clear();
            elementStack.clear();

            while(true) {
                char token = readByte();
                int tType = token & 0x0f;
                int dType = token & 0xf0;

                if(tType == TOKEN_START_DOCUMENT) {
                    if(dType != DATA_NULL || docOpen) {
                        std::cerr << "START_DOCUMENT with an invalid data type" << std::endl;
                        goto fail;
                    }
                    docOpen = true;
                } else if(tType == TOKEN_END_DOCUMENT){
                    if(dType != DATA_NULL || elementStack.size() != 0) {
                        std::cerr << "END_DOCUMENT with an invalid data type" << std::endl;
                        goto fail;
                    }
                    break;
                } else if(tType == TOKEN_START_TAG) {
                    if(dType != DATA_STRING_INTERNED || !docOpen || rootClosed) {
                        std::cerr << "An error occurred after START_TAG received" << std::endl;
                        goto fail;
                    }

                    std::vector<char> intStr1 = readInternedString();
                    XMLElement* element = new XMLElement(intStr1);
                    if(elementStack.size() == 0) {
                        elementStack.push_back(element);
                    } else {
                        XMLElement* last1 = elementStack.back();
                        last1->subElements.push_back(element);
                        elementStack.push_back(element);
                    }
                } else if(tType == TOKEN_END_TAG){
                    if(dType != DATA_STRING_INTERNED || !docOpen || rootClosed || elementStack.size() < 1) {
                        std::cerr << "An error occurred after END_TAG received" << std::endl;
                        goto fail;
                    }

                    std::vector<char> intStr2 = readInternedString();
                    
                    XMLElement* last2 = elementStack.back();
                    char* ch1 = reinterpret_cast<char*>(last2->mTagName.data());
                    char* ch2 = reinterpret_cast<char*>(intStr2.data());
                    if(strcmp(ch1, ch2) != 0) {
                        std::cerr << "START_TAG and END_TAG mismatch" << std::endl;
                        std::cerr << ch1 << " - " << ch2 << std::endl;
                        // goto fail;
                    }

                    if((elementStack.size()-1) < 1) {
                        rootClosed = true;
                        root = *last2;
                    }

                    elementStack.pop_back();
                } else if(tType == TOKEN_TEXT){
                    std::vector<char> raw1 = readString();
                    XMLElement last2 = *elementStack.back();
                    last2.text.insert(last2.text.end(), raw1.begin(), raw1.end());
                } else if(tType == TOKEN_ATTRIBUTE){
                    if(elementStack.size() < 1) {
                        std::cerr << "ATTRIBUTE without any elements left" << std::endl;
                        goto fail;
                    }

                    XMLElement last3 = *elementStack.back();
                    std::vector<char> attrName = readInternedString();
                    const char* attrNameArr = reinterpret_cast<const char*>(attrName.data());
                    for(const auto &it : (*last3.attributes)) {
                        const char* first = reinterpret_cast<const char*>(it.first.data());
                        if(strcmp(first, attrNameArr) == 0) {
                            std::cerr << "ATTRIBUTE " << attrNameArr << " already in target element" << std::endl;
                            goto fail;
                        }
                    }

                    std::vector<char> value;
                    if(dType == DATA_NULL) {
                        // nop
                    } else if(dType == DATA_BOOLEAN_TRUE) {
                        const char* temp = "true";
                        value.insert(value.begin(), temp, temp + 4);
                    } else if(dType == DATA_BOOLEAN_FALSE) {
                        const char* temp = "false";
                        value.insert(value.begin(), temp, temp + 5);
                    } else if(dType == DATA_INT || dType == DATA_INT_HEX) {
                        int* read = readInt();
                        if(read) {
                            char* out = new char[32];
                            sprintf(out, "%d", *read);
                            value.assign(out, out + 32);
                        }
                    } else if(dType == DATA_LONG || dType == DATA_LONG_HEX) {
                        long* read = readLong();
                        if(read) {
                            char* out = new char[64];
                            sprintf(out, "%ld", *read);
                            value.assign(out, out + 64);
                        }
                    } else if(dType == DATA_FLOAT) {
                        float* read = readFloat();
                        if(read) {
                            std::string out = std::to_string(*read);
                            value.assign(out.begin(), out.end());
                        }
                    } else if(dType == DATA_DOUBLE) {
                        double* read = readDouble();
                        if(read) {
                            std::string out = std::to_string(*read);
                            value.assign(out.begin(), out.end());
                        }
                    } else if(dType == DATA_STRING) {
                        std::vector<char> temp = readString();
                        if(temp.size() > 0)
                            value.assign(temp.begin(), temp.end());
                    } else if(dType == DATA_STRING_INTERNED) {
                        std::vector<char> temp = readInternedString();
                        if(temp.size() > 0)
                            value.assign(temp.begin(), temp.end());
                    } else if(dType == DATA_BYTES_HEX) {
                        short* len = readShort();
                        if(len) {
                            std::vector<char> buf = readFromCurPos(*len);
                            char* temp = new char[(*len)*2];
                            short pos = 0;
                            while(pos < *len) {
                                sprintf(temp, "%02x", buf[pos]);
                                pos++;
                            }
                            value.insert(value.begin(), temp, temp + (*len));
                            free(temp);
                        }
                    } else if(dType == DATA_BYTES_BASE64) {
                        short* len = readShort();
                        if(len) {
                            std::vector<char> buf = readFromCurPos(*len);
                            value = base64_encode(buf, *len);
                        }
                    } else {
                        std::cerr << "Invalid dType " << dType;
                        goto fail;
                    }

                    if(value.size() < 1) {
                        std::string empty = " ";
                        value.assign(empty.begin(), empty.end());
                    }

                    XMLAttribute* attrs = new XMLAttribute(dType, value);
                    (*last3.attributes).insert({attrName, attrs});
                } else
                    std::cerr << "Unsupported token type " << tType << " " << dType << std::endl;
            }

            if(!rootClosed) {
                std::cerr << "Elements still in the stack when completing the document" << std::endl;
                goto fail;
            }

            parseElement(&root);
            return &parsedElement;

            fail:
            return NULL;
        }

        AbxDecoder(std::vector<char> str){
            mInput = str;
        }
    private:
        const short
            TOKEN_START_DOCUMENT = 0,
            TOKEN_END_DOCUMENT = 1,
            TOKEN_START_TAG = 2,
		    TOKEN_END_TAG = 3,
		    TOKEN_TEXT = 4,
		    TOKEN_ATTRIBUTE = 15;

        const short
            DATA_NULL = 1 << 4,
		    DATA_STRING = 2 << 4,
		    DATA_STRING_INTERNED = 3 << 4,
		    DATA_BYTES_HEX = 4 << 4,
		    DATA_BYTES_BASE64 = 5 << 4,
		    DATA_INT = 6 << 4,
		    DATA_INT_HEX = 7 << 4,
		    DATA_LONG = 8 << 4,
		    DATA_LONG_HEX = 9 << 4,
		    DATA_FLOAT = 10 << 4,
		    DATA_DOUBLE = 11 << 4,
		    DATA_BOOLEAN_TRUE = 12 << 4,
		    DATA_BOOLEAN_FALSE = 13 << 4;

        int curPos = 0;
        std::vector<char> mInput;
        const char* startMagic = new char[]{'A', 'B', 'X', '\0'};
        std::string parsedElement;
        XMLElement root;
        std::list<std::vector<char>> internedStrings;
        std::list<XMLElement*> elementStack;
        bool docOpen = false, rootClosed = false;

        std::vector<char> readFromCurPos(int len) {
            std::vector ret(mInput.begin() + curPos, mInput.begin() + curPos + len);
            curPos += len;
            return ret;
        }

        char readByte() {
            return readFromCurPos(1)[0];
        }

        short* readShort() {
            std::vector<char> off = readFromCurPos(2);
            if(off.size() != 2) return NULL;

            short* nv = (short*) malloc(sizeof(short));
            nv[0] = off[1] | (short) off[0] << 8;
            return nv;
        }

        int* readInt() {
            std::vector<char> off = readFromCurPos(4);
            if(off.size() != 4) return NULL;

            int ret = 0;
            for(int i = 0;i < 4;i++) {
                ret |= (int) (off[3-i] & 0xff) << (i * 8);
            }
            int* nv = (int*) malloc(sizeof(int));
            nv[0] = ret;
            return nv;
        }

        long* readLong() {
            std::vector<char> high = readFromCurPos(4);
            std::vector<char> low = readFromCurPos(4);
            if((high.size() + low.size()) != 8) return NULL;

            int reth = 0, retl = 0;
            for(int i = 0;i < 4;i++) {
                reth |= (int) ((high[3-i] & 0xff) << (i * 8));
            }

            for(int i = 0;i < 4;i++) {
                retl |= (int) ((low[3-i] & 0xff) << (i * 8));
            }

            long ret = (((long) reth) << 32L) | (((long) retl) & 0xffffffffL);
            long* nv = (long*) malloc(sizeof(int));
            nv[0] = ret;
            return nv;
        }

        float* readFloat() {
            std::vector<char> off = readFromCurPos(4);
            if(off.size() != 4) return NULL;

            uint32_t ret = 0;
            for(int i = 0;i < 4;i++) {
                ret |= (int) off[3-i] << (i * 8);
            }

            float* re2 = reinterpret_cast<float*>(&ret);
            return re2;
        }

        double* readDouble() {
            std::vector<char> off = readFromCurPos(8);
            if(off.size() != 8) return NULL;

            uint64_t ret = 0;
            for(int i = 0;i < 8;i++) {
                ret = ret | (long) (off[7-i] << (i * 8));
            }
            
            double* re2 = reinterpret_cast<double*>(&ret);
            return re2;
        }

        std::vector<char> readString() {
            short* len = readShort();
            if(!len || *len < 1) {
                return *(new std::vector<char>());
            }

            if(*len < 1) {
                std::cerr << "Negative or zero string length detected\n";
                len = 0;
            }

            auto ret = readFromCurPos(*len);
            return ret;
        }

        std::vector<char> readInternedString() {
            short* ref = readShort();
            if(!ref) {
                return *(new std::vector<char>());
            }
            
            if(*ref == -1) {
                std::vector<char> str = readString();
                internedStrings.push_back(str);
                return str;
            }

            auto internedStr = internedStrings.begin();
            std::advance(internedStr, *ref);
            return *internedStr;
        }

        const std::string base64_chars =
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+/";

        std::vector<char> base64_encode(std::vector<char> buf, int bufLen) {
            std::vector<char> ret;
            int i = 0, j = 0;
            char char_array_3[3];
            char char_array_4[4];

            while (bufLen--) {
                char_array_3[i++] = buf.at(buf.size() - bufLen);
                if (i == 3) {
                    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                    char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                    char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                    char_array_4[3] = char_array_3[2] & 0x3f;

                    char* temp = new char[4];
                    for(i = 0; (i <4) ; i++)
                        temp[i] = base64_chars[char_array_4[i]];
                    ret.insert(ret.end(), temp, temp + 4);
                    free(temp);

                    i = 0;
                }
            }

            if (i){
                for(j = i; j < 3; j++)
                    char_array_3[j] = '\0';

                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;

                char* temp = new char[i + 1];
                for (j = 0; (j < i + 1); j++)
                    temp[j] = base64_chars[char_array_4[j]];
                ret.insert(ret.end(), temp, temp + 4);
                free(temp);
                
                temp = (char*) "=";
                while((i++ < 3))
                    ret.insert(ret.end(), temp, temp + 1);
            }

            return ret;
        }

        inline std::vector<char> trim(std::vector<char>& str){
            if(str.size() > 0) {
                std::string nstr(str.begin(), str.end());
                nstr.erase(nstr.find_last_not_of(' ')+1);
                nstr.erase(0, nstr.find_first_not_of(' '));
                str.clear();
                str.assign(nstr.begin(), nstr.end());
            }
            return str;
        }

        void parseElement(XMLElement* element) {
            parsedElement += ("<");
            parsedElement.insert(parsedElement.end(), (*element).mTagName.begin(), (*element).mTagName.end());

            if((*(*element).attributes).size() > 0) {
                for(const auto &it : (*(*element).attributes)) {
                    parsedElement += (" ");
                    parsedElement += (it.first.data());
                    parsedElement += ("=\"");
                    std::vector<char> trimmedVal = trim(it.second->mValue);
                    if(trimmedVal.size() > 0)
                        parsedElement += (trimmedVal.data());
                    parsedElement += ("\"");
                }
            }

            if((*element).text.size() > 0) {
                if((*element).subElements.size() > 0) {
                    std::cerr << "SubElement and Text in same time" << std::endl;
                    return;
                }

                parsedElement += (">");
                parsedElement.insert(parsedElement.end(), (*element).text.begin(), (*element).text.end());
                parsedElement += ("</");
                parsedElement.insert(parsedElement.end(), (*element).mTagName.begin(), (*element).mTagName.end());
                parsedElement += (">");
            }

            if((*element).subElements.size() > 0) {
                parsedElement += (">\n");
                for(int i = 0;i < (*element).subElements.size();i++) {
                    auto item = (*element).subElements.begin();
                    std::advance(item, i);
                    parseElement(*item);
                }
                parsedElement += ("</");
                parsedElement.insert(parsedElement.end(), (*element).mTagName.begin(), (*element).mTagName.end());
                parsedElement += (">");
            } else {
                parsedElement += ("/>\n");
            }
        }
};
