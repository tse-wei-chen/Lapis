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
