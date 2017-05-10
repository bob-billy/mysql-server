/* Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include "default_values.h"

#include <string.h>
#include <sys/types.h>
#include <algorithm>

#include "binary_log_types.h"
#include "dd/properties.h"     // dd::Properties
#include "dd/string_type.h"
#include "dd/types/column.h"   // dd::Column
#include "dd/types/table.h"    // dd::Table
#include "dd_table_share.h"    // dd_get_old_field_type
#include "field.h"             // calc_pack_length
#include "handler.h"           // handler
#include "item.h"              // Item
#include "my_base.h"
#include "my_compare.h"
#include "my_dbug.h"
#include "my_decimal.h"        // DECIMAL_MAX_SCALE
#include "my_macros.h"
#include "my_pointer_arithmetic.h"
#include "my_sys.h"
#include "mysql_com.h"
#include "mysqld_error.h"
#include "sql_class.h"         // THD
#include "sql_list.h"          // List
#include "table.h"

/**
  Calculate the length of the in-memory representation of the column.

  This function calculates the amount of memory necessary to store values
  of the submitted column. The function is used when preparing the default
  values for the columns of a table, and for determining the size of an
  empty record for the table which the column is a part of.

  @note The implementation is based on Create_field::init() and
        Create_field::create_length_to_internal_length().

  @param  col_obj   The column object for which we calculate the
                    in-memory length.

  @return           In-memory length of values of the submitted column.
*/

static size_t column_pack_length(const dd::Column &col_obj)
{
  size_t pack_length= 0;

  switch (col_obj.type()) {
  case dd::enum_column_types::TINY_BLOB:
  case dd::enum_column_types::MEDIUM_BLOB:
  case dd::enum_column_types::LONG_BLOB:
  case dd::enum_column_types::BLOB:
  case dd::enum_column_types::GEOMETRY:
  case dd::enum_column_types::VAR_STRING:
  case dd::enum_column_types::STRING:
  case dd::enum_column_types::VARCHAR:
    // The length is already calculated in number of bytes, no need
    // to multiply by number of bytes per symbol.
    pack_length= calc_pack_length(dd_get_old_field_type(col_obj.type()),
                                  col_obj.char_length());
    break;
  case dd::enum_column_types::ENUM:
    pack_length= get_enum_pack_length(col_obj.elements_count());
    break;
  case dd::enum_column_types::SET:
    pack_length= get_set_pack_length(col_obj.elements_count());
    break;
  case dd::enum_column_types::BIT:
    {
      bool treat_bit_as_char;
      if (col_obj.options().get_bool("treat_bit_as_char", &treat_bit_as_char))
        DBUG_ASSERT(false); /* purecov: deadcode */

      if (treat_bit_as_char)
        pack_length= ((col_obj.char_length() + 7) & ~7) / 8;
      else
        pack_length= col_obj.char_length() / 8;
    }
    break;
  case dd::enum_column_types::NEWDECIMAL:
    {
      uint decimals= col_obj.numeric_scale();
      ulong precision= my_decimal_length_to_precision(col_obj.char_length(),
                                                      decimals,
                                                      col_obj.is_unsigned());
      set_if_smaller(precision, DECIMAL_MAX_PRECISION);
      DBUG_ASSERT((precision <= DECIMAL_MAX_PRECISION) &&
                  (decimals <= DECIMAL_MAX_SCALE));
      pack_length= my_decimal_get_binary_size(precision, decimals);
    }
    break;
  default:
    pack_length= calc_pack_length(dd_get_old_field_type(col_obj.type()),
                                  col_obj.char_length());
    break;
  }
  return pack_length;
}


/**
  Find the required length of an empty record.

  This function iterates over the columns of the table, finds the required
  number of null bits and leftover bits, and adds up the total length of an
  empty record. Various length related fields in the table share parameter
  are assigned.

  @param       table         A single table, with column data members.
  @param       min_length    The minimum length of a record.
  @param [out] share         Table share with various length fields assigned.

  @retval      true          Failure.
  @retval      false         Success.
*/

static bool find_record_length(const dd::Table &table, size_t min_length,
                               TABLE_SHARE *share)
{
  // Get the table property 'pack_record' and initialize out parameters.
  bool pack_record;
  if (table.options().get_bool("pack_record", &pack_record))
    return true;

  DBUG_ASSERT(share);
  share->fields= 0;
  share->null_fields= 0;
  share->reclength= 0;
  ulong leftover_bits= pack_record ? 0 : 1;

  // Loop over columns, count nullable and bit fields and find record length.
  for (const dd::Column *col_obj : table.columns())
  {
    // Skip hidden columns
    if (col_obj->is_hidden())
      continue;

    // Check if the field may be NULL.
    if (col_obj->is_nullable())
      share->null_fields++;

    // Check if this is a BIT field with leftover bits in the preamble, and
    // adjust record length accordingly.
    if (col_obj->type() == dd::enum_column_types::BIT)
    {
      bool treat_bit_as_char;
      if (col_obj->options().get_bool("treat_bit_as_char", &treat_bit_as_char))
        return true;

      if (! treat_bit_as_char && (col_obj->char_length() & 7))
        leftover_bits+= col_obj->char_length() & 7;
    }

    // Increment record length.
    share->reclength+= column_pack_length(*col_obj);
    share->fields++;
  }

  // Find preamble length and add it to the total record length.
  share->null_bytes= (share->null_fields + leftover_bits + 7) / 8;
  share->last_null_bit_pos= (share->null_fields + leftover_bits) & 7;
  share->reclength+= share->null_bytes;

  // Hack to avoid bugs with small static rows in MySQL.
  share->reclength= std::max<size_t>(min_length, share->reclength);
  share->stored_rec_length= share->reclength;

  return false;
}


/**
  Set the unused bits in the preamble of a default value buffer.

  This function will set the unused bits, after the preamble bits and
  up to the first byte border, to 1. It  will also set the 'pack record'
  bit (the first bit in the preamble).

  The number of null bits here is assumed to include the number of bit field
  bits that do not fit into a whole byte (i.e., number of bits modulo 8) for
  storage engines that store bits like this in the preamble.

  @param          pack_record    If the HA_OPTION_PACK_RECORD is set.
  @param          preamble_bits  Number of null bits and leftover bits.
  @param [in,out] default_values The default value buffer.
*/

static void set_pack_record_and_unused_preamble_bits(bool pack_record,
                                                     ulong preamble_bits,
                                                     uchar *default_values)
{
  DBUG_ASSERT(default_values);

  // Set first bit if the HA_OPTION_PACK_RECORD is not set.
  if (!pack_record)
    *default_values|= 1;

  // We need to set the unused bits to 1. If the number of bits is a multiple
  // of 8 there are no unused bits.
  if (preamble_bits & 7)
    *(default_values + preamble_bits / 8)|=
            ~(((uchar) 1 << (preamble_bits & 7)) - 1);
}


size_t max_pack_length(const List<Create_field> &create_fields)
{
  size_t max_pack_length= 0;
  // Iterate over the create fields and find the largest one.
  List_iterator<Create_field> field_it(
                                const_cast<List<Create_field>&>(create_fields));
  Create_field *field;
  while ((field= field_it++))
    max_pack_length= std::max<size_t>(field->pack_length, max_pack_length);
  return max_pack_length;
}


bool prepare_default_value(THD *thd, uchar *buf, const TABLE &table,
                           const Create_field &field, dd::Column *col_obj)
{
  // Create a fake field with a real data buffer in which to store the value.
  Field *regfield= make_field(table.s,
                              buf + 1,
                              field.length,
                              buf,
                              0,
                              field.sql_type,
                              field.charset,
                              field.geom_type,
                              field.auto_flags,
                              field.interval,
                              field.field_name,
                              field.maybe_null,
                              field.is_zerofill,
                              field.is_unsigned,
                              field.decimals,
                              field.treat_bit_as_char,
                              field.pack_length_override);
  bool retval= true;
  if (!regfield)
    goto err;

  // save_in_field() will access regfield->table->in_use.
  regfield->init(const_cast<TABLE*>(&table));

  // Set if the field may be NULL.
  if (!(field.flags & NOT_NULL_FLAG))
    regfield->set_null();

  if (field.def)
  {
    // Pointless to store the value of a function as it may not be constant.
    DBUG_ASSERT(field.def->type() != Item::FUNC_ITEM);
    type_conversion_status res= field.def->save_in_field(regfield, true);
    if (res != TYPE_OK && res != TYPE_NOTE_TIME_TRUNCATED &&
        res != TYPE_NOTE_TRUNCATED)
    {
      // Clear current error and report ER_INVALID_DEFAULT.
      if (thd->is_error())
        thd->clear_error();

      my_error(ER_INVALID_DEFAULT, MYF(0), regfield->field_name);
      goto err;
    }
  }
  else if (regfield->real_type() == MYSQL_TYPE_ENUM &&
           (field.flags & NOT_NULL_FLAG))
  {
    regfield->set_notnull();
    regfield->store((longlong) 1, TRUE);
  }
  else
    regfield->reset();

  // Save whether column has only type's implicit default value.
  // Note that in this case that we still store this default value
  // in DD to preserve backward compatibility.
  col_obj->set_has_no_default((field.flags & NO_DEFAULT_VALUE_FLAG));

  // Save NULL flag, default value and leftover bits.
  col_obj->set_default_value_null(regfield->is_null());
  if (!col_obj->is_default_value_null())
  {
    dd::String_type default_value;
    default_value.assign(reinterpret_cast<char*>(buf + 1), field.pack_length);

    // Append leftover bits as the last byte of the default value.
    if (field.sql_type == MYSQL_TYPE_BIT &&
        !field.treat_bit_as_char && (field.length & 7))
    {
      // Downcast and get bits.
      Field_bit *bitfield= dynamic_cast<Field_bit*>(regfield);
      // In get_rec_bits(), bitfield->bit_ptr[1] is accessed, so we must be
      // sure that the buffer is at least two bytes long. This is ensured
      // at the point where the buffer is allocated.
      uchar default_preamble_bits= get_rec_bits(bitfield->bit_ptr,
                                                bitfield->bit_ofs,
                                                bitfield->bit_len);
      default_value.append(reinterpret_cast<char*>(&default_preamble_bits), 1);
    }
    col_obj->set_default_value(default_value);
  }
  retval= false;

err:
  // Destroy the field, despite being MEM_ROOT allocated, to avoid memory
  // leak for fields that allocate extra memory (e.g Field_blob::value).
  destroy(regfield);
  return retval;
}


bool prepare_default_value_buffer_and_table_share(THD *thd,
                                                  const dd::Table &table,
                                                  TABLE_SHARE *share)
{
  DBUG_ASSERT(share);

  // Get the handler temporarily, needed to get minimal record length as
  // well as extra record length.
  handler *file= NULL;
  handlerton *engine= share->db_type();
  if (!(file= get_new_handler(NULL, table.partition_type()!=dd::Table::PT_NONE,
                              thd->mem_root, engine)))
  {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR),
             static_cast<int>(sizeof(handler)));
    return true;
  }

  // Get the minimal and extra record buffer lengths from the handler.
  size_t extra_length= file->extra_rec_buf_length();
  size_t min_length= static_cast<size_t>(file->min_record_length(
          share->db_create_options));
  destroy(file);

  // Get the number of columns, record length etc.
  if (find_record_length(table, min_length, share))
    return true;

  // Adjust buffer size and allocate the default value buffer.
  share->rec_buff_length= ALIGN_SIZE(share->reclength + 1 + extra_length);
  if (share->reclength)
  {
    share->default_values= reinterpret_cast<uchar*>(alloc_root(&share->mem_root,
                                                      share->rec_buff_length));
    if (!share->default_values)
      return true;

    // Initialize the default value buffer. The default values for the
    // columns are assigned when each individual column is initialized,
    // in 'Table_share_utils::fill_column_from_dd()'.
    memset(share->default_values, 0, share->reclength);

    // Find the number of used bits in the preamble.
    ulong preamble_bits= share->last_null_bit_pos;
    if (share->null_bytes)
      preamble_bits+= (share->null_bytes - 1 ) * 8;

    set_pack_record_and_unused_preamble_bits((share->db_create_options &
                                              HA_OPTION_PACK_RECORD),
                                             preamble_bits,
                                             share->default_values);
  }

  return false;
}
