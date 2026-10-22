using Lapis.Contracts.Media;

namespace Lapis.Library;

/// <summary>
/// Catalog-only contract. Import indexes the existing path; media bytes are
/// never copied, transcoded, decrypted or moved by the library.
/// </summary>
public interface IMediaLibrary
{
    Task<IReadOnlyList<MediaLibraryItem>> ListAsync(CancellationToken cancellationToken = default);
    Task<MediaLibraryItem> ImportAsync(MediaLocator locator, CancellationToken cancellationToken = default);
    Task<bool> RemoveAsync(Guid id, CancellationToken cancellationToken = default);
}

/// <summary>
/// Optional catalog capability for repairing moved/offline media sources.
/// Keeps the logical movie ID stable so watch progress and favorites remain
/// attached to the same title. Implementations must validate new locations
/// before atomically persisting the change.
/// </summary>
public interface IRelinkableMediaLibrary : IMediaLibrary
{
    Task<MediaLibraryItem> RelinkAsync(Guid itemId, MediaLocator replacement,
        CancellationToken cancellationToken = default);
}

/// <summary>
/// Optional catalog capability for editing user-facing movie labels.
/// Renaming changes display metadata only, not a media source or its ID.
/// </summary>
public interface IEditableMediaLibrary : IMediaLibrary
{
    Task<MediaLibraryItem> RenameAsync(Guid itemId, string title,
        CancellationToken cancellationToken = default);
}
