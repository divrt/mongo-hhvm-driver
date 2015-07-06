/**
 *  Copyright 2015 MongoDB, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "hphp/runtime/vm/native-data.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/type-string.h"

#include "bson.h"
#include "utils.h"
#include <iostream>

#include "src/MongoDB/BSON/Binary.h"
#include "src/MongoDB/BSON/Javascript.h"
#include "src/MongoDB/BSON/ObjectID.h"
#include "src/MongoDB/BSON/Regex.h"
#include "src/MongoDB/BSON/Timestamp.h"
#include "src/MongoDB/BSON/UTCDateTime.h"

extern "C" {
#include "libbson/src/bson/bson.h"
#include "libmongoc/src/mongoc/mongoc.h"
}

namespace HPHP {
/* {{{ HHVM → BSON */
int VariantToBsonConverter::_isPackedArray(const Array &a)
{
	int idx = 0, key_value = 0;

	for (ArrayIter iter(a); iter; ++iter) {
		Variant key(iter.first());

		if (!key.isInteger()) {
			return false;
		}

		key_value = key.toInt32(); 

		if (idx != key_value) {
			return false;
		}

		idx++;
	}
	return true;
}

VariantToBsonConverter::VariantToBsonConverter(const Variant& document, int flags)
{
	m_document = document;
	m_level = 0;
	m_flags = flags;
	m_out = NULL;
}

void VariantToBsonConverter::convert(bson_t *bson)
{
	if (m_document.isObject() || m_document.isArray()) {
		convertDocument(bson, NULL, m_document);
	} else {
		std::cout << "convert *unimplemented*: " << getDataTypeString(m_document.getType()).c_str() << "\n";
	}
}

void VariantToBsonConverter::convertElement(bson_t *bson, const char *key, Variant v)
{
	switch (v.getType()) {
		case KindOfUninit:
		case KindOfNull:
			convertNull(bson, key);
			break;
		case KindOfBoolean:
			convertBoolean(bson, key, v.toBoolean());
			break;
		case KindOfInt64:
			convertInt64(bson, key, v.toInt64());
			break;
		case KindOfDouble:
			convertDouble(bson, key, v.toDouble());
			break;
		case KindOfStaticString:
		case KindOfString:
			convertString(bson, key, v.toString());
			break;
		case KindOfArray:
		case KindOfObject:
			convertDocument(bson, key, v);
			break;
		case KindOfResource:
			throw MongoDriver::Utils::throwUnexpectedValueException("Got unsupported type 'resource'");
			return;
		default:
			break;
	}
}

void VariantToBsonConverter::convertNull(bson_t *bson, const char *key)
{
	bson_append_null(bson, key, -1);
};

void VariantToBsonConverter::convertBoolean(bson_t *bson, const char *key, bool v)
{
	bson_append_bool(bson, key, -1, v);
};

void VariantToBsonConverter::convertInt64(bson_t *bson, const char *key, int64_t v)
{
	if (v > INT_MAX || v < INT_MIN) {
		bson_append_int64(bson, key, -1, v);
	} else {
		bson_append_int32(bson, key, -1, (int32_t) v);
	}
};

void VariantToBsonConverter::convertDouble(bson_t *bson, const char *key, double v)
{
	bson_append_double(bson, key, -1, v);
};

void VariantToBsonConverter::convertString(bson_t *bson, const char *key, String v)
{
	bson_append_utf8(bson, key, -1, v.c_str(), v.size());
}

char *VariantToBsonConverter::_getUnmangledPropertyName(String key)
{
	const char *ckey = key.c_str();

	if (ckey[0] == '\0' && key.length()) {
		const char *cls = ckey + 1;
		if (*cls == '*') { // protected
			return strdup(ckey + 3);
		} else {
			int l = strlen(cls);
			return strdup(cls + l + 1);
		}
	} else {
		return strdup(ckey);
	}
}

void VariantToBsonConverter::convertDocument(bson_t *bson, const char *property_name, Variant v)
{
	bson_t child;
	int unmangle = 0;
	Array document;

	/* if we are not at a top-level, we need to check (and convert) special
	 * BSON types too */
	if (v.isObject()) {
		if (convertSpecialObject(bson, property_name, v.toObject())) {
			return;
		}
		/* The "convertSpecialObject" method didn't understand this type, so we
		 * will continue treating this as a normal document */
	}

	document = v.toArray();

	if (_isPackedArray(document) && !v.isObject()) {
		if (property_name != NULL) {
			bson_append_array_begin(bson, property_name, -1, &child);
		}
	} else {
		unmangle = 1;
		if (property_name != NULL) {
			bson_append_document_begin(bson, property_name, -1, &child);
		}
	}

	for (ArrayIter iter(document); iter; ++iter) {
		Variant key(iter.first());
		const Variant& data(iter.secondRef());
		String s_key = key.toString();

		if (m_level == 0 && (m_flags & HIPPO_BSON_ADD_ID)) {

			if (strncmp(s_key.c_str(), "_id", s_key.length()) == 0) {
				m_flags &= !HIPPO_BSON_ADD_ID;
			}
		}

		m_level++;
		if (unmangle) {
			const char *unmangledName;

			unmangledName = _getUnmangledPropertyName(s_key);
			convertElement(property_name != NULL ? &child : bson, unmangledName, data);
			free((void*) unmangledName);
		} else {
			convertElement(property_name != NULL ? &child : bson, s_key.c_str(), data);
		}
		m_level--;
	}

	if (m_level == 0 && (m_flags & HIPPO_BSON_ADD_ID)) {
		bson_oid_t oid;

		bson_oid_init(&oid, NULL);
		bson_append_oid(bson, "_id", strlen("_id"), &oid);

		if (m_flags & HIPPO_BSON_RETURN_ID) {
			m_out = bson_new();
			bson_append_oid(m_out, "_id", sizeof("_id")-1, &oid);
		}
	}

	if (property_name != NULL) {
		if (_isPackedArray(document)) {
			bson_append_array_end(bson, &child);
		} else {
			bson_append_document_end(bson, &child);
		}
	}
}

/* {{{ Serialization of types */
const StaticString s_MongoDriverBsonType_className("MongoDB\\BSON\\Type");
const StaticString s_MongoDriverBsonPersistable_className("MongoDB\\BSON\\Persistable");
const StaticString s_MongoDriverBsonSerializable_className("MongoDB\\BSON\\Serializable");
const StaticString s_MongoDriverBsonUnserializable_className("MongoDB\\BSON\\Unserializable");
const StaticString s_MongoDriverBsonSerializable_functionName("bsonSerialize");
const StaticString s_MongoDriverBsonUnserializable_functionName("bsonUnserialize");
const StaticString s_MongoDriverBsonODM_fieldName("__pclass");

/* {{{ MongoDriver\BSON\Binary */
void VariantToBsonConverter::_convertBinary(bson_t *bson, const char *key, Object v)
{
	String data = v.o_get(s_MongoBsonBinary_data, false, s_MongoBsonBinary_className);
	int64_t subType = v.o_get(s_MongoBsonBinary_subType, false, s_MongoBsonBinary_className).toInt64();

	bson_append_binary(bson, key, -1, (bson_subtype_t) subType, (const unsigned char*) data.c_str(), data.length());
}
/* }}} */

/* {{{ MongoDriver\BSON\Javascript */
void VariantToBsonConverter::_convertJavascript(bson_t *bson, const char *key, Object v)
{
	bson_t *scope_bson;
	String code = v.o_get(s_MongoBsonJavascript_code, false, s_MongoBsonJavascript_className);
	auto scope = v.o_get(s_MongoBsonJavascript_scope, false, s_MongoBsonJavascript_className);

	if (scope.isObject() || scope.isArray()) {
		/* Convert scope to document */
		VariantToBsonConverter converter(scope, HIPPO_BSON_NO_FLAGS);
		scope_bson = bson_new();
		converter.convert(scope_bson);

		bson_append_code_with_scope(bson, key, -1, (const char*) code.c_str(), scope_bson);
	} else {
		bson_append_code(bson, key, -1, (const char*) code.c_str());
	}
}
/* }}} */

/* {{{ MongoDriver\BSON\MaxKey */
const StaticString s_MongoBsonMaxKey_className("MongoDB\\BSON\\MaxKey");

void VariantToBsonConverter::_convertMaxKey(bson_t *bson, const char *key, Object v)
{
	bson_append_maxkey(bson, key, -1);
}
/* }}} */

/* {{{ MongoDriver\BSON\MinKey */
const StaticString s_MongoBsonMinKey_className("MongoDB\\BSON\\MinKey");

void VariantToBsonConverter::_convertMinKey(bson_t *bson, const char *key, Object v)
{
	bson_append_minkey(bson, key, -1);
}
/* }}} */

/* {{{ MongoDriver\BSON\ObjectID */
void VariantToBsonConverter::_convertObjectID(bson_t *bson, const char *key, Object v)
{
    MongoDBBsonObjectIDData* data = Native::data<MongoDBBsonObjectIDData>(v.get());

	bson_append_oid(bson, key, -1, &data->m_oid);
}
/* }}} */

/* {{{ MongoDriver\BSON\Regex */

void VariantToBsonConverter::_convertRegex(bson_t *bson, const char *key, Object v)
{
	String regex = v.o_get(s_MongoBsonRegex_pattern, false, s_MongoBsonRegex_className);
	String flags = v.o_get(s_MongoBsonRegex_flags, false, s_MongoBsonRegex_className);

	bson_append_regex(bson, key, -1, regex.c_str(), flags.c_str());
}
/* }}} */

/* {{{ MongoDriver\BSON\Timestamp */
void VariantToBsonConverter::_convertTimestamp(bson_t *bson, const char *key, Object v)
{
	int32_t timestamp = v.o_get(s_MongoBsonTimestamp_timestamp, false, s_MongoBsonTimestamp_className).toInt32();
	int32_t increment = v.o_get(s_MongoBsonTimestamp_increment, false, s_MongoBsonTimestamp_className).toInt32();

	bson_append_timestamp(bson, key, -1, timestamp, increment);
}

/* {{{ MongoDriver\BSON\UTCDateTime */
void VariantToBsonConverter::_convertUTCDateTime(bson_t *bson, const char *key, Object v)
{
	int64_t milliseconds = v.o_get(s_MongoBsonUTCDateTime_milliseconds, false, s_MongoBsonUTCDateTime_className).toInt64();

	bson_append_date_time(bson, key, -1, milliseconds);
}
/* }}} */

/* {{{ MongoDriver\BSON\UTCDateTime */
void VariantToBsonConverter::_convertSerializable(bson_t *bson, const char *key, Object v)
{
	Variant result;
	Array   properties;
	TypedValue args[1] = { *(Variant(v)).asCell() };
	Class *cls;
	Func *m;

	cls = v.get()->getVMClass();
	m = cls->lookupMethod(s_MongoDriverBsonSerializable_functionName.get());
	
	g_context->invokeFuncFew(
		result.asTypedValue(),
		m,
		v.get(),
		nullptr,
		1, args
	);

	if (!result.isArray()) {
		StringBuffer buf;
		buf.printf("Expected %s() to return an array, but %s given", s_MongoDriverBsonSerializable_functionName.data(), HPHP::getDataTypeString(result.getType()).data());
		Variant full_name = buf.detach();

		throw MongoDriver::Utils::throwUnexpectedValueException((char*) full_name.toString().c_str());
	}

	properties = result.toArray();

	if (v.instanceof(s_MongoDriverBsonPersistable_className)) {
		properties.add(String(s_MongoDriverBsonODM_fieldName), String(cls->nameStr()));
	}

	convertDocument(bson, key, properties);
}
/* }}} */

/* }}} */

bool VariantToBsonConverter::convertSpecialObject(bson_t *bson, const char *key, Object v)
{
	if (v.instanceof(s_MongoDriverBsonType_className)) {
		if (v.instanceof(s_MongoBsonBinary_className)) {
			_convertBinary(bson, key, v);
			return true;
		}
		if (v.instanceof(s_MongoBsonJavascript_className)) {
			_convertJavascript(bson, key, v);
			return true;
		}
		if (v.instanceof(s_MongoBsonMaxKey_className)) {
			_convertMaxKey(bson, key, v);
			return true;
		}
		if (v.instanceof(s_MongoBsonMinKey_className)) {
			_convertMinKey(bson, key, v);
			return true;
		}
		if (v.instanceof(s_MongoBsonObjectID_className)) {
			_convertObjectID(bson, key, v);
			return true;
		}
		if (v.instanceof(s_MongoBsonRegex_className)) {
			_convertRegex(bson, key, v);
			return true;
		}
		if (v.instanceof(s_MongoBsonTimestamp_className)) {
			_convertTimestamp(bson, key, v);
			return true;
		}
		if (v.instanceof(s_MongoBsonUTCDateTime_className)) {
			_convertUTCDateTime(bson, key, v);
			return true;
		}
		if (v.instanceof(s_MongoDriverBsonSerializable_className)) {
			_convertSerializable(bson, key, v);
			return true;
		}
	}
	return false;
}
/* }}} */

/* {{{ BSON → HHVM */
BsonToVariantConverter::BsonToVariantConverter(const unsigned char *data, int data_len, hippo_bson_conversion_options_t options)
{
	m_reader = bson_reader_new_from_data(data, data_len);
	m_options = options;
}

/* {{{ Visitors */
void hippo_bson_visit_corrupt(const bson_iter_t *iter __attribute__((unused)), void *data)
{
	std::cout << "converting corrupt\n";
}

bool hippo_bson_visit_double(const bson_iter_t *iter __attribute__((unused)), const char *key, double v_double, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;

	state->zchild.add(String(key), Variant(v_double));
	return false;
}

bool hippo_bson_visit_utf8(const bson_iter_t *iter __attribute__((unused)), const char *key, size_t v_utf8_len, const char *v_utf8, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;

	state->zchild.add(String(key), Variant(v_utf8));
	return false;
}

bool hippo_bson_visit_document(const bson_iter_t *iter __attribute__((unused)), const char *key, const bson_t *v_document, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;
	Variant document_v;

	BsonToVariantConverter converter(bson_get_data(v_document), v_document->len, state->options);
	converter.convert(&document_v);

	if (state->options.document_type == HIPPO_TYPEMAP_ARRAY) {
		state->zchild.add(String(key), document_v);
	} else {
		state->zchild.add(String(key), Variant(Variant(document_v).toObject()));
	}

	return false;
}

bool hippo_bson_visit_array(const bson_iter_t *iter __attribute__((unused)), const char *key, const bson_t *v_array, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;
	Variant array_v;

	BsonToVariantConverter converter(bson_get_data(v_array), v_array->len, state->options);
	converter.convert(&array_v);

	if (state->options.array_type == HIPPO_TYPEMAP_ARRAY) {
		state->zchild.add(String(key), array_v.toArray());
	} else {
		state->zchild.add(String(key), Variant(Variant(array_v).toObject()));
	}

	return false;
}

bool hippo_bson_visit_binary(const bson_iter_t *iter __attribute__((unused)), const char *key, bson_subtype_t v_subtype, size_t v_binary_len, const uint8_t *v_binary, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;
	static Class* c_binary;
	String s;
	unsigned char *data_s;

	s = String(v_binary_len, ReserveString);
	data_s = (unsigned char*) s.bufferSlice().ptr;
	memcpy(data_s, v_binary, v_binary_len);
	s.setSize(v_binary_len);

	c_binary = Unit::lookupClass(s_MongoBsonBinary_className.get());
	assert(c_binary);
	ObjectData* obj = ObjectData::newInstance(c_binary);

	obj->o_set(s_MongoBsonBinary_data, s, s_MongoBsonBinary_className.get());
	obj->o_set(s_MongoBsonBinary_subType, Variant(v_subtype), s_MongoBsonBinary_className.get());

	state->zchild.add(String(key), Variant(obj));

	return false;
}

bool hippo_bson_visit_oid(const bson_iter_t *iter __attribute__((unused)), const char *key, const bson_oid_t *v_oid, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;
	static Class* c_objectId;

	c_objectId = Unit::lookupClass(s_MongoBsonObjectID_className.get());
	assert(c_objectId);
	ObjectData* obj = ObjectData::newInstance(c_objectId);

	MongoDBBsonObjectIDData* obj_data = Native::data<MongoDBBsonObjectIDData>(obj);
	bson_oid_copy(v_oid, &obj_data->m_oid);

	state->zchild.add(String(key), Variant(obj));

	return false;
}

bool hippo_bson_visit_bool(const bson_iter_t *iter __attribute__((unused)), const char *key, bool v_bool, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;

	state->zchild.add(String(key), Variant(v_bool));
	return false;
}

bool hippo_bson_visit_date_time(const bson_iter_t *iter __attribute__((unused)), const char *key, int64_t msec_since_epoch, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;
	static Class* c_datetime;

	c_datetime = Unit::lookupClass(s_MongoBsonUTCDateTime_className.get());
	assert(c_datetime);
	ObjectData* obj = ObjectData::newInstance(c_datetime);

	obj->o_set(s_MongoBsonUTCDateTime_milliseconds, Variant(msec_since_epoch), s_MongoBsonUTCDateTime_className.get());

	state->zchild.add(String(key), Variant(obj));

	return false;
}

bool hippo_bson_visit_null(const bson_iter_t *iter __attribute__((unused)), const char *key, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;

	state->zchild.add(String(key), Variant(Variant::NullInit()));
	return false;
}

bool hippo_bson_visit_regex(const bson_iter_t *iter __attribute__((unused)), const char *key, const char *v_regex, const char *v_options, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;
	static Class* c_regex;

	c_regex = Unit::lookupClass(s_MongoBsonRegex_className.get());
	assert(c_regex);
	ObjectData* obj = ObjectData::newInstance(c_regex);

	obj->o_set(s_MongoBsonRegex_pattern, Variant(v_regex), s_MongoBsonRegex_className.get());
	obj->o_set(s_MongoBsonRegex_flags, Variant(v_options), s_MongoBsonRegex_className.get());

	state->zchild.add(String(key), Variant(obj));

	return false;
}

bool hippo_bson_visit_code(const bson_iter_t *iter __attribute__((unused)), const char *key, size_t v_code_len, const char *v_code, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;
	static Class* c_code;
	String s;
	unsigned char *data_s;

	s = String(v_code_len, ReserveString);
	data_s = (unsigned char*) s.bufferSlice().ptr;
	memcpy(data_s, v_code, v_code_len);
	s.setSize(v_code_len);

	c_code = Unit::lookupClass(s_MongoBsonJavascript_className.get());
	assert(c_code);
	ObjectData* obj = ObjectData::newInstance(c_code);

	obj->o_set(s_MongoBsonJavascript_code, s, s_MongoBsonJavascript_className.get());

	state->zchild.add(String(key), Variant(obj));

	return false;
}

bool hippo_bson_visit_codewscope(const bson_iter_t *iter __attribute__((unused)), const char *key, size_t v_code_len, const char *v_code, const bson_t *v_scope, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;
	static Class* c_code;
	String s;
	unsigned char *data_s;
	Variant scope_v;

	/* code */
	s = String(v_code_len, ReserveString);
	data_s = (unsigned char*) s.bufferSlice().ptr;
	memcpy(data_s, v_code, v_code_len);
	s.setSize(v_code_len);

	/* scope */
	BsonToVariantConverter converter(bson_get_data(v_scope), v_scope->len, state->options);
	converter.convert(&scope_v);

	/* create object */
	c_code = Unit::lookupClass(s_MongoBsonJavascript_className.get());
	assert(c_code);
	ObjectData* obj = ObjectData::newInstance(c_code);

	/* set properties */
	obj->o_set(s_MongoBsonJavascript_code, s, s_MongoBsonJavascript_className.get());
	obj->o_set(s_MongoBsonJavascript_scope, scope_v, s_MongoBsonJavascript_className.get());

	/* add to array */
	state->zchild.add(String(key), Variant(obj));

	return false;
}

bool hippo_bson_visit_int32(const bson_iter_t *iter __attribute__((unused)), const char *key, int32_t v_int32, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;

	state->zchild.add(String(key), Variant(v_int32));
	return false;
}

bool hippo_bson_visit_timestamp(const bson_iter_t *iter __attribute__((unused)), const char *key, uint32_t v_timestamp, uint32_t v_increment, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;
	static Class* c_timestamp;

	c_timestamp = Unit::lookupClass(s_MongoBsonTimestamp_className.get());
	assert(c_timestamp);
	ObjectData* obj = ObjectData::newInstance(c_timestamp);

	obj->o_set(s_MongoBsonTimestamp_timestamp, Variant((uint64_t) v_timestamp), s_MongoBsonTimestamp_className.get());
	obj->o_set(s_MongoBsonTimestamp_increment, Variant((uint64_t) v_increment), s_MongoBsonTimestamp_className.get());

	state->zchild.add(String(key), Variant(obj));

	return false;
}

bool hippo_bson_visit_int64(const bson_iter_t *iter __attribute__((unused)), const char *key, int64_t v_int64, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;

	state->zchild.add(String(key), Variant(v_int64));
	return false;
}

bool hippo_bson_visit_maxkey(const bson_iter_t *iter __attribute__((unused)), const char *key, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;
	static Class* c_objectId;

	c_objectId = Unit::lookupClass(s_MongoBsonMaxKey_className.get());
	assert(c_objectId);
	ObjectData* obj = ObjectData::newInstance(c_objectId);

	state->zchild.add(String(key), Variant(obj));

	return false;
}

bool hippo_bson_visit_minkey(const bson_iter_t *iter __attribute__((unused)), const char *key, void *data)
{
	hippo_bson_state *state = (hippo_bson_state*) data;
	static Class* c_objectId;

	c_objectId = Unit::lookupClass(s_MongoBsonMinKey_className.get());
	assert(c_objectId);
	ObjectData* obj = ObjectData::newInstance(c_objectId);

	state->zchild.add(String(key), Variant(obj));

	return false;
}


static const bson_visitor_t hippo_bson_visitors = {
	NULL /* hippo_phongo_bson_visit_before*/,
	NULL /*hippo_phongo_bson_visit_after*/,
	hippo_bson_visit_corrupt,
	hippo_bson_visit_double,
	hippo_bson_visit_utf8,
	hippo_bson_visit_document,
	hippo_bson_visit_array,
	hippo_bson_visit_binary,
	NULL /*hippo_bson_visit_undefined*/,
	hippo_bson_visit_oid,
	hippo_bson_visit_bool,
	hippo_bson_visit_date_time,
	hippo_bson_visit_null,
	hippo_bson_visit_regex,
	NULL /*hippo_bson_visit_dbpointer*/,
	hippo_bson_visit_code,
	NULL /*hippo_bson_visit_symbol*/,
	hippo_bson_visit_codewscope,
	hippo_bson_visit_int32,
	hippo_bson_visit_timestamp,
	hippo_bson_visit_int64,
	hippo_bson_visit_maxkey,
	hippo_bson_visit_minkey,
	{ NULL }
};
/* }}} */


bool BsonToVariantConverter::convert(Variant *v)
{
	bson_iter_t   iter;
	bool          eof = false;
	const bson_t *b;
	hippo_bson_state state;

	state.zchild = NULL;

	if (!(b = bson_reader_read(m_reader, &eof))) {
//		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not read document from reader");
		return false;
	}

	do {
		if (!bson_iter_init(&iter, b)) {
			bson_reader_destroy(m_reader);
			return false;
		}

		state.zchild = Array::Create();
		state.options = m_options;

		bson_iter_visit_all(&iter, &hippo_bson_visitors, &state);
/*
		if (state->map.array || state->odm) {
			zval *obj = NULL;

			MAKE_STD_ZVAL(obj); 
			object_init_ex(obj, state->odm ? state->odm : state->map.array);
			zend_call_method_with_1_params(&obj, NULL, NULL, BSON_UNSERIALIZE_FUNC_NAME, NULL, state->zchild);
			zval_dtor(state->zchild);
			ZVAL_ZVAL(state->zchild, obj, 1, 1);
		}
*/
	} while ((b = bson_reader_read(m_reader, &eof)));

	if (m_options.document_type == HIPPO_TYPEMAP_STDCLASS) {
		*v = Variant(Variant(state.zchild).toObject());
	} else {
		*v = Variant(state.zchild);
	}

	return true;
}
/* }}} */


} /* namespace HPHP */
