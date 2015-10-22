#pragma once

#include <DB/Common/StringSearcher.h>
#include <Poco/UTF8Encoding.h>
#include <Poco/Unicode.h>
#include <ext/range.hpp>
#include <x86intrin.h>
#include <stdint.h>
#include <string.h>


/** Поиск подстроки в строке по алгоритму Вольницкого:
  * http://volnitsky.com/project/str_search/
  *
  * haystack и needle могут содержать нулевые байты.
  *
  * Алгоритм:
  * - при слишком маленьком или слишком большом размере needle, или слишком маленьком haystack, используем std::search или memchr;
  * - при инициализации, заполняем open-addressing linear probing хэш-таблицу вида:
  *    хэш от биграммы из needle -> позиция этой биграммы в needle + 1.
  *    (прибавлена единица только чтобы отличить смещение ноль от пустой ячейки)
  * - в хэш-таблице ключи не хранятся, хранятся только значения;
  * - биграммы могут быть вставлены несколько раз, если они встречаются в needle несколько раз;
  * - при поиске, берём из haystack биграмму, которая должна соответствовать последней биграмме needle (сравниваем с конца);
  * - ищем её в хэш-таблице, если нашли - достаём смещение из хэш-таблицы и сравниваем строку побайтово;
  * - если сравнить не получилось - проверяем следующую ячейку хэш-таблицы из цепочки разрешения коллизий;
  * - если не нашли, пропускаем в haystack почти размер needle байт;
  *
  * Используется невыровненный доступ к памяти.
  */


namespace DB
{


/// @todo store lowercase needle to speed up in case there are numerous occurrences of bigrams from needle in haystack
template <typename CRTP>
class VolnitskyBase
{
protected:
	using offset_t = uint8_t;	/// Смещение в needle. Для основного алгоритма, длина needle не должна быть больше 255.
	using ngram_t = uint16_t;	/// n-грамма (2 байта).

	const UInt8 * const needle;
	const size_t needle_size;
	const UInt8 * const needle_end = needle + needle_size;
	/// На сколько двигаемся, если n-грамма из haystack не нашлась в хэш-таблице.
	const size_t step = needle_size - sizeof(ngram_t) + 1;

	/** max needle length is 255, max distinct ngrams for case-sensitive is (255 - 1), case-insensitive is 4 * (255 - 1)
	 *	storage of 64K ngrams (n = 2, 128 KB) should be large enough for both cases */
	static const size_t hash_size = 64 * 1024;	/// Помещается в L2-кэш.
	offset_t hash[hash_size];	/// Хэш-таблица.

	/// min haystack size to use main algorithm instead of fallback
	static constexpr auto min_haystack_size_for_algorithm = 20000;
	const bool fallback;				/// Нужно ли использовать fallback алгоритм.

public:
	/** haystack_size_hint - ожидаемый суммарный размер haystack при вызовах search. Можно не указывать.
	  * Если указать его достаточно маленьким, то будет использован fallback алгоритм,
	  *  так как считается, что тратить время на инициализацию хэш-таблицы не имеет смысла.
	  */
	VolnitskyBase(const char * const needle, const size_t needle_size, size_t haystack_size_hint = 0)
	: needle{reinterpret_cast<const UInt8 *>(needle)}, needle_size{needle_size},
	  fallback{
		  needle_size < 2 * sizeof(ngram_t) or needle_size >= std::numeric_limits<offset_t>::max() or
		  (haystack_size_hint and haystack_size_hint < min_haystack_size_for_algorithm)
	  }
	{
		if (fallback)
			return;

		memset(hash, 0, sizeof(hash));

		/// int is used here because unsigned can't be used with condition like `i >= 0`, unsigned always >= 0
		for (auto i = static_cast<int>(needle_size - sizeof(ngram_t)); i >= 0; --i)
			self().putNGram(this->needle + i, i + 1);
	}


	/// Если не найдено - возвращается конец haystack.
	const UInt8 * search(const UInt8 * const haystack, const size_t haystack_size) const
	{
		if (needle_size == 0)
			return haystack;

		const auto haystack_end = haystack + haystack_size;

		if (needle_size == 1 || fallback || haystack_size <= needle_size)
			return self().search_fallback(haystack, haystack_end);

		/// Будем "прикладывать" needle к haystack и сравнивать n-грам из конца needle.
		const auto * pos = haystack + needle_size - sizeof(ngram_t);
		for (; pos <= haystack_end - needle_size; pos += step)
		{
			/// Смотрим все ячейки хэш-таблицы, которые могут соответствовать n-граму из haystack.
			for (size_t cell_num = toNGram(pos) % hash_size; hash[cell_num];
				 cell_num = (cell_num + 1) % hash_size)
			{
				/// Когда нашли - сравниваем побайтово, используя смещение из хэш-таблицы.
				const auto res = pos - (hash[cell_num] - 1);

				if (self().compare(res))
					return res;
			}
		}

		/// Оставшийся хвостик.
		return self().search_fallback(pos - step + 1, haystack_end);
	}

	const char * search(const char * haystack, size_t haystack_size) const
	{
		return reinterpret_cast<const char *>(search(reinterpret_cast<const UInt8 *>(haystack), haystack_size));
	}

protected:
	CRTP & self() { return static_cast<CRTP &>(*this); }
	const CRTP & self() const { return const_cast<VolnitskyBase *>(this)->self(); }

	static const ngram_t & toNGram(const UInt8 * const pos)
	{
		return *reinterpret_cast<const ngram_t *>(pos);
	}

	void putNGramBase(const ngram_t ngram, const int offset)
	{
		/// Кладём смещение для n-грама в соответствующую ему ячейку или ближайшую свободную.
		size_t cell_num = ngram % hash_size;

		while (hash[cell_num])
			cell_num = (cell_num + 1) % hash_size; /// Поиск следующей свободной ячейки.

		hash[cell_num] = offset;
	}
};


template <bool CaseSensitive, bool ASCII> struct VolnitskyImpl;

/// Case sensitive comparison
template <bool ASCII> struct VolnitskyImpl<true, ASCII> : VolnitskyBase<VolnitskyImpl<true, ASCII>>
{
	VolnitskyImpl(const char * const needle, const size_t needle_size, const size_t haystack_size_hint = 0)
		: VolnitskyBase<VolnitskyImpl<true, ASCII>>{needle, needle_size, haystack_size_hint},
		  fallback_searcher{needle, needle_size}
	{
	}

	void putNGram(const UInt8 * const pos, const int offset)
	{
		this->putNGramBase(this->toNGram(pos), offset);
	}

	bool compare(const UInt8 * const pos) const
	{
		/// @todo: maybe just use memcmp for this case and rely on internal SSE optimization as in case with memcpy?
		return fallback_searcher.compare(pos);
	}

	const UInt8 * search_fallback(const UInt8 * const haystack, const UInt8 * const haystack_end) const
	{
		return fallback_searcher.search(haystack, haystack_end);
	}

	ASCIICaseSensitiveStringSearcher fallback_searcher;
};

/// Case-insensitive ASCII
template <> struct VolnitskyImpl<false, true> : VolnitskyBase<VolnitskyImpl<false, true>>
{
	VolnitskyImpl(const char * const needle, const size_t needle_size, const size_t haystack_size_hint = 0)
		: VolnitskyBase{needle, needle_size, haystack_size_hint}, fallback_searcher{needle, needle_size}
	{
	}

	void putNGram(const UInt8 * const pos, const int offset)
	{
		union {
			ngram_t n;
			UInt8 c[2];
		};

		n = toNGram(pos);
		const auto c0_alpha = std::isalpha(c[0]);
		const auto c1_alpha = std::isalpha(c[1]);

		if (c0_alpha && c1_alpha)
		{
			/// 4 combinations: AB, aB, Ab, ab
			c[0] = std::tolower(c[0]);
			c[1] = std::tolower(c[1]);
			putNGramBase(n, offset);

			c[0] = std::toupper(c[0]);
			putNGramBase(n, offset);

			c[1] = std::toupper(c[1]);
			putNGramBase(n, offset);

			c[0] = std::tolower(c[0]);
			putNGramBase(n, offset);
		}
		else if (c0_alpha)
		{
			/// 2 combinations: A1, a1
			c[0] = std::tolower(c[0]);
			putNGramBase(n, offset);

			c[0] = std::toupper(c[0]);
			putNGramBase(n, offset);
		}
		else if (c1_alpha)
		{
			/// 2 combinations: 0B, 0b
			c[1] = std::tolower(c[1]);
			putNGramBase(n, offset);

			c[1] = std::toupper(c[1]);
			putNGramBase(n, offset);
		}
		else
			/// 1 combination: 01
			putNGramBase(n, offset);
	}

	bool compare(const UInt8 * const pos) const
	{
		return fallback_searcher.compare(pos);
	}

	const UInt8 * search_fallback(const UInt8 * const haystack, const UInt8 * const haystack_end) const
	{
		return fallback_searcher.search(haystack, haystack_end);
	}

	ASCIICaseInsensitiveStringSearcher fallback_searcher;
};

/// Case-sensitive UTF-8
template <> struct VolnitskyImpl<false, false> : VolnitskyBase<VolnitskyImpl<false, false>>
{
	VolnitskyImpl(const char * const needle, const size_t needle_size, const size_t haystack_size_hint = 0)
		: VolnitskyBase{needle, needle_size, haystack_size_hint}, fallback_searcher{needle, needle_size}
	{
	}

	void putNGram(const UInt8 * const pos, const int offset)
	{
		union
		{
			ngram_t n;
			UInt8 c[2];
		};

		n = toNGram(pos);

		if (isascii(c[0]) && isascii(c[1]))
		{
			const auto c0_al = std::isalpha(c[0]);
			const auto c1_al = std::isalpha(c[1]);

			if (c0_al && c1_al)
			{
				/// 4 combinations: AB, aB, Ab, ab
				c[0] = std::tolower(c[0]);
				c[1] = std::tolower(c[1]);
				putNGramBase(n, offset);

				c[0] = std::toupper(c[0]);
				putNGramBase(n, offset);

				c[1] = std::toupper(c[1]);
				putNGramBase(n, offset);

				c[0] = std::tolower(c[0]);
				putNGramBase(n, offset);
			}
			else if (c0_al)
			{
				/// 2 combinations: A1, a1
				c[0] = std::tolower(c[0]);
				putNGramBase(n, offset);

				c[0] = std::toupper(c[0]);
				putNGramBase(n, offset);
			}
			else if (c1_al)
			{
				/// 2 combinations: 0B, 0b
				c[1] = std::tolower(c[1]);
				putNGramBase(n, offset);

				c[1] = std::toupper(c[1]);
				putNGramBase(n, offset);
			}
			else
				/// 1 combination: 01
				putNGramBase(n, offset);
		}
		else
		{
			using Seq = UInt8[6];

			static const Poco::UTF8Encoding utf8;

			if (UTF8::isContinuationOctet(c[1]))
			{
				/// ngram is inside a sequence
				auto seq_pos = pos;
				UTF8::syncBackward(seq_pos);

				const auto u32 = utf8.convert(seq_pos);
				const auto l_u32 = Poco::Unicode::toLower(u32);
				const auto u_u32 = Poco::Unicode::toUpper(u32);

				/// symbol is case-independent
				if (l_u32 == u_u32)
					putNGramBase(n, offset);
				else
				{
					/// where is the given ngram in respect to UTF-8 sequence start?
					const auto seq_ngram_offset = pos - seq_pos;

					Seq seq;

					/// put ngram from lowercase
					utf8.convert(l_u32, seq, sizeof(seq));
					c[0] = seq[seq_ngram_offset];
					c[1] = seq[seq_ngram_offset + 1];
					putNGramBase(n, offset);

					/// put ngram for uppercase
					utf8.convert(u_u32, seq, sizeof(seq));
					c[0] = seq[seq_ngram_offset];
					c[1] = seq[seq_ngram_offset + 1];
					putNGramBase(n, offset);
				}
			}
			else
			{
				/// ngram is on the boundary of two sequences
				/// first sequence may start before u_pos if it is not ASCII
				auto first_seq_pos = pos;
				UTF8::syncBackward(first_seq_pos);

				const auto first_u32 = utf8.convert(first_seq_pos);
				const auto first_l_u32 = Poco::Unicode::toLower(first_u32);
				const auto first_u_u32 = Poco::Unicode::toUpper(first_u32);

				/// second sequence always start immediately after u_pos
				auto second_seq_pos = pos + 1;

				const auto second_u32 = utf8.convert(second_seq_pos);
				const auto second_l_u32 = Poco::Unicode::toLower(second_u32);
				const auto second_u_u32 = Poco::Unicode::toUpper(second_u32);

				/// both symbols are case-independent
				if (first_l_u32 == first_u_u32 && second_l_u32 == second_u_u32)
					putNGramBase(n, offset);
				else if (first_l_u32 == first_u_u32)
				{
					/// first symbol is case-independent
					Seq seq;

					/// put ngram for lowercase
					utf8.convert(second_l_u32, seq, sizeof(seq));
					c[1] = seq[0];
					putNGramBase(n, offset);

					/// put ngram from uppercase
					utf8.convert(second_u_u32, seq, sizeof(seq));
					c[1] = seq[0];
					putNGramBase(n, offset);
				}
				else if (second_l_u32 == second_u_u32)
				{
					/// second symbol is case-independent

					/// where is the given ngram in respect to the first UTF-8 sequence start?
					const auto seq_ngram_offset = pos - first_seq_pos;

					Seq seq;

					/// put ngram for lowercase
					utf8.convert(second_l_u32, seq, sizeof(seq));
					c[0] = seq[seq_ngram_offset];
					putNGramBase(n, offset);

					/// put ngram for uppercase
					utf8.convert(second_u_u32, seq, sizeof(seq));
					c[0] = seq[seq_ngram_offset];
					putNGramBase(n, offset);
				}
				else
				{
					/// where is the given ngram in respect to the first UTF-8 sequence start?
					const auto seq_ngram_offset = pos - first_seq_pos;

					Seq first_l_seq, first_u_seq, second_l_seq, second_u_seq;

					utf8.convert(first_l_u32, first_l_seq, sizeof(first_l_seq));
					utf8.convert(first_u_u32, first_u_seq, sizeof(first_u_seq));
					utf8.convert(second_l_u32, second_l_seq, sizeof(second_l_seq));
					utf8.convert(second_u_u32, second_u_seq, sizeof(second_u_seq));

					/// ngram for ll
					c[0] = first_l_seq[seq_ngram_offset];
					c[1] = second_l_seq[0];
					putNGramBase(n, offset);

					/// ngram for lU
					c[0] = first_l_seq[seq_ngram_offset];
					c[1] = second_u_seq[0];
					putNGramBase(n, offset);

					/// ngram for Ul
					c[0] = first_u_seq[seq_ngram_offset];
					c[1] = second_l_seq[0];
					putNGramBase(n, offset);

					/// ngram for UU
					c[0] = first_u_seq[seq_ngram_offset];
					c[1] = second_u_seq[0];
					putNGramBase(n, offset);
				}
			}
		}
	}

	bool compare(const UInt8 * const pos) const
	{
		return fallback_searcher.compare(pos);
	}

	const UInt8 * search_fallback(const UInt8 * const haystack, const UInt8 * const haystack_end) const
	{
		return fallback_searcher.search(haystack, haystack_end);
	}

	UTF8CaseInsensitiveStringSearcher fallback_searcher;
};


using Volnitsky = VolnitskyImpl<true, true>;
using VolnitskyUTF8 = VolnitskyImpl<true, false>;	/// exactly same as Volnitsky
using VolnitskyCaseInsensitive = VolnitskyImpl<false, true>;	/// ignores non-ASCII bytes
using VolnitskyCaseInsensitiveUTF8 = VolnitskyImpl<false, false>;


}
