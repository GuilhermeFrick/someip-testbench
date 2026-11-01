/**
 * @file      InstanceSpecifier.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Validation of a meta-model path.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   The validation is what makes the private constructor worth having: every specifier in
 *            the system passed through here, so no consumer has to check a path again.
 *
 * @details   A meta-model path is a sequence of short names separated by @c '/'. The specification
 *            constrains a short name to a C-like identifier: a letter or underscore, then letters,
 *            digits and underscores. That is what is checked, and nothing more — this subset does
 *            not know the meta-model, so it cannot tell whether a well formed path names anything
 *            that exists. Resolution is the platform's job, and here it belongs to whoever consumes
 *            the specifier; see ::ara::idsm::EventReporter::Create().
 */
#include "src/ara/core/InstanceSpecifier.hpp"

#include "src/ara/core/CoreErrorDomain.hpp"

namespace ara
{
namespace core
{

namespace
{

/**
 * @brief Whether a character may start a short name.
 * @param[in] c Character to classify.
 * @return Whether @p c is a letter or an underscore.
 * @details Written against the ASCII ranges rather than with @c std::isalpha, which depends on the
 *          locale: a meta-model path means the same thing in every locale.
 */
bool StartsShortName(char c)
{
    return (((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) || (c == '_'));
}

/**
 * @brief Whether a character may continue a short name.
 * @param[in] c Character to classify.
 * @return Whether @p c is a letter, a digit or an underscore.
 */
bool ContinuesShortName(char c)
{
    return (StartsShortName(c) || ((c >= '0') && (c <= '9')));
}

/**
 * @brief Whether every segment of a path is a well formed short name.
 * @param[in] path Path to check; the caller guarantees it is not empty.
 * @return Whether the path is made of non-empty, well formed segments separated by one @c '/'.
 * @details Rejects an empty segment, which is what a leading, trailing or doubled separator
 *          produces. A path such as @c "a//b" is the typical result of concatenating a prefix that
 *          already ends in a separator, and it is better refused at construction than carried
 *          around as a path that resolves to nothing.
 */
bool SegmentsAreShortNames(const std::string &path)
{
    bool bom      = true;
    bool atStart  = true; /* The next character begins a segment. */
    for (std::size_t i = 0U; (i < path.size()) && bom; ++i)
    {
        const char c = path[i];
        if (c == '/')
        {
            /* An empty segment: either the path starts with the separator, or two
             * separators came in a row. */
            bom     = !atStart;
            atStart = true;
        }
        else if (atStart)
        {
            bom     = StartsShortName(c);
            atStart = false;
        }
        else
        {
            bom = ContinuesShortName(c);
        }
    }
    /* A trailing separator leaves a segment open, which is also an empty segment. */
    return (bom && !atStart);
}

} // namespace

Result<InstanceSpecifier> InstanceSpecifier::Create(const std::string &metaModelIdentifier)
{
    using Res = Result<InstanceSpecifier>;

    if (metaModelIdentifier.empty())
    {
        return Res::FromError(ErrorCode(CoreErrc::kInvalidMetaModelPath));
    }
    else
    {
        /* There is something to validate. */
    }

    if (!SegmentsAreShortNames(metaModelIdentifier))
    {
        /* The path is syntactically a path, but some segment is not a short name.
         * The specification distinguishes the two, so the code does as well. */
        return Res::FromError(ErrorCode(CoreErrc::kInvalidMetaModelShortname));
    }
    else
    {
        /* Every segment is a well formed short name. */
    }

    return Res::FromValue(InstanceSpecifier(metaModelIdentifier));
}

} // namespace core
} // namespace ara
